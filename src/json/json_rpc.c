#include <nsr/json/json_rpc.h>
#include <nsr/json/json.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <poll.h>
#include <sys/wait.h>
#include <time.h>

bool nsr_json_rpc_spawn(nsr_json_rpc_t *rpc, const char *path)
{
    memset(rpc, 0, sizeof(*rpc));
    rpc->next_id = 1;
    rpc->pending_id = -1;
    nsr_json_init(&rpc->rx_buf);

    int in_pipe[2] = { -1, -1 };
    int out_pipe[2] = { -1, -1 };
    if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0) {
        if (in_pipe[0] >= 0) close(in_pipe[0]);
        if (in_pipe[1] >= 0) close(in_pipe[1]);
        nsr_json_free(&rpc->rx_buf);
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        nsr_json_free(&rpc->rx_buf);
        return false;
    }

    if (pid == 0) {
        /* Child */
        close(in_pipe[1]);
        close(out_pipe[0]);
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[0]);
        close(out_pipe[1]);
        execl(path, path, (char *)nullptr);
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    rpc->pid = pid;
    rpc->in = in_pipe[1];
    rpc->out = out_pipe[0];
    return true;
}

void nsr_json_rpc_close(nsr_json_rpc_t *rpc)
{
    if (!rpc)
        return;
    if (rpc->in >= 0) {
        close(rpc->in);
        rpc->in = -1;
    }
    if (rpc->out >= 0) {
        close(rpc->out);
        rpc->out = -1;
    }
    if (rpc->pid > 0) {
        kill(rpc->pid, SIGTERM);
        waitpid(rpc->pid, nullptr, 0);
        rpc->pid = 0;
    }
    nsr_json_free(&rpc->rx_buf);
    rpc->dead = true;
    rpc->pending_id = -1;
}

static bool rpc_send(nsr_json_rpc_t *rpc, const nsr_json_buf_t *req)
{
    if (rpc->dead || rpc->in < 0)
        return false;
    const char *s = nsr_json_cstr(req);
    size_t len = strlen(s);
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(rpc->in, s + off, len - off);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            rpc->dead = true;
            return false;
        }
        off += (size_t)n;
    }
    if (write(rpc->in, "\n", 1) != 1) {
        rpc->dead = true;
        return false;
    }
    return true;
}

static bool build_request(const char *method,
                          const nsr_json_buf_t *params,
                          long long id,
                          nsr_json_buf_t *req)
{
    nsr_json_init(req);
    nsr_json_obj_start(req);
    nsr_json_key(req, "jsonrpc");
    nsr_json_string(req, "2.0");
    nsr_json_key(req, "id");
    nsr_json_int(req, id);
    nsr_json_key(req, "method");
    nsr_json_string(req, method);
    nsr_json_key(req, "params");
    if (params && params->len > 0)
        nsr_json_append_raw(req, nsr_json_cstr(params), params->len);
    else
        nsr_json_obj_start(req), nsr_json_obj_end(req);
    nsr_json_obj_end(req);
    return true;
}

bool nsr_json_rpc_send_request(nsr_json_rpc_t *rpc,
                               const char *method,
                               const nsr_json_buf_t *params,
                               long long *id_out)
{
    if (rpc->dead || rpc->in < 0)
        return false;

    long long id = rpc->next_id++;
    if (id_out)
        *id_out = id;
    rpc->pending_id = id;

    nsr_json_buf_t req;
    build_request(method, params, id, &req);
    bool ok = rpc_send(rpc, &req);
    nsr_json_free(&req);
    if (!ok) {
        rpc->pending_id = -1;
        return false;
    }
    return true;
}

/* Read one line from rpc->out into buf. Returns true if a full line was read,
 * false on error/EOF. The newline is not stored in buf. */
static bool recv_line(nsr_json_rpc_t *rpc, nsr_json_buf_t *buf, int timeout_ms)
{
    struct pollfd pfd = { .fd = rpc->out, .events = POLLIN };
    struct timespec deadline;
    bool has_deadline = false;

    if (timeout_ms >= 0) {
        if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0)
            return false;
        deadline.tv_sec += timeout_ms / 1000;
        deadline.tv_nsec += (timeout_ms % 1000) * 1000000LL;
        if (deadline.tv_nsec >= 1000000000LL) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000LL;
        }
        has_deadline = true;
    }

    while (1) {
        if (rpc->rx_buf.len > 0) {
            char *nl = memchr(rpc->rx_buf.buf, '\n', rpc->rx_buf.len);
            if (nl) {
                size_t line_len = (size_t)(nl - rpc->rx_buf.buf);
                size_t rest_off = line_len + 1;
                size_t rest_len = rpc->rx_buf.len - rest_off;

                nsr_json_append_raw(buf, rpc->rx_buf.buf, line_len);
                memmove(rpc->rx_buf.buf, rpc->rx_buf.buf + rest_off, rest_len);
                rpc->rx_buf.len = rest_len;
                rpc->rx_buf.buf[rest_len] = '\0';
                return true;
            }
        }

        int poll_timeout = 50;
        if (has_deadline) {
            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
                return false;
            long long left = ((long long)deadline.tv_sec - (long long)now.tv_sec) * 1000LL +
                             ((long long)deadline.tv_nsec - (long long)now.tv_nsec) / 1000000LL;
            if (left <= 0)
                return false;
            poll_timeout = left > 50 ? 50 : (int)left;
        }

        int rc = poll(&pfd, 1, poll_timeout);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            rpc->dead = true;
            return false;
        }
        if (rc == 0) {
            if (has_deadline) {
                struct timespec now;
                if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
                    return false;
                if ((long long)now.tv_sec > (long long)deadline.tv_sec ||
                    ((long long)now.tv_sec == (long long)deadline.tv_sec &&
                     (long long)now.tv_nsec >= (long long)deadline.tv_nsec))
                    return false;
            }
            /* Check if child died. */
            int status;
            if (waitpid(rpc->pid, &status, WNOHANG) != 0) {
                rpc->dead = true;
                return false;
            }
            continue;
        }

        char tmp[4096];
        ssize_t n = read(rpc->out, tmp, sizeof(tmp));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            rpc->dead = true;
            return false;
        }
        if (n == 0) {
            rpc->dead = true;
            return false;
        }
        nsr_json_append_raw(&rpc->rx_buf, tmp, (size_t)n);
    }
}

bool nsr_json_rpc_try_recv_response(nsr_json_rpc_t *rpc,
                                    int timeout_ms,
                                    nsr_json_buf_t *response_out)
{
    if (rpc->dead || rpc->out < 0 || rpc->pending_id < 0)
        return false;

    nsr_json_buf_t buf;
    nsr_json_init(&buf);

    int deadline = timeout_ms;
    while (1) {
        int slice = deadline > 50 ? 50 : deadline;
        if (deadline < 0)
            slice = 50;

        if (!recv_line(rpc, &buf, slice)) {
            deadline -= slice;
            if (deadline <= 0 && timeout_ms >= 0) {
                nsr_json_free(&buf);
                return false;
            }
            if (rpc->dead) {
                nsr_json_free(&buf);
                return false;
            }
            continue;
        }

        /* We have a line. Verify it matches the pending request id. */
        size_t len;
        const char *idv = nsr_json_obj_get(nsr_json_cstr(&buf), "id", &len);
        long long resp_id = -1;
        if (idv)
            nsr_json_parse_int(idv, len, &resp_id);

        if (resp_id == rpc->pending_id) {
            *response_out = buf;
            rpc->pending_id = -1;
            return true;
        }

        /* Mismatched/rogue line: discard and keep polling within the budget. */
        nsr_json_reset(&buf);
        deadline -= slice;
        if (deadline <= 0 && timeout_ms >= 0) {
            nsr_json_free(&buf);
            return false;
        }
    }
}

bool nsr_json_rpc_try_recv_any(nsr_json_rpc_t *rpc,
                               int timeout_ms,
                               nsr_json_buf_t *response_out,
                               long long *id_out)
{
    if (rpc->dead || rpc->out < 0)
        return false;

    nsr_json_buf_t buf;
    nsr_json_init(&buf);

    if (!recv_line(rpc, &buf, timeout_ms)) {
        nsr_json_free(&buf);
        return false;
    }

    size_t len;
    const char *idv = nsr_json_obj_get(nsr_json_cstr(&buf), "id", &len);
    long long resp_id = -1;
    if (idv)
        nsr_json_parse_int(idv, len, &resp_id);

    if (id_out)
        *id_out = resp_id;
    *response_out = buf;
    return true;
}

void nsr_json_rpc_discard_queued(nsr_json_rpc_t *rpc)
{
    const int max_reads = 64;

    if (!rpc)
        return;

    nsr_json_reset(&rpc->rx_buf);

    if (rpc->dead || rpc->out < 0)
        return;

    for (int reads = 0; reads < max_reads; reads++) {
        struct pollfd pfd = { .fd = rpc->out, .events = POLLIN };
        int rc = poll(&pfd, 1, 0);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            rpc->dead = true;
            return;
        }
        if (rc == 0)
            return;

        char tmp[4096];
        ssize_t n = read(rpc->out, tmp, sizeof(tmp));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            rpc->dead = true;
            return;
        }
        if (n == 0) {
            rpc->dead = true;
            return;
        }
    }
}

bool nsr_json_rpc_call(nsr_json_rpc_t *rpc,
                       const char *method,
                       const nsr_json_buf_t *params,
                       int timeout_ms,
                       nsr_json_buf_t *response_out)
{
    if (!nsr_json_rpc_send_request(rpc, method, params, nullptr))
        return false;
    return nsr_json_rpc_try_recv_response(rpc, timeout_ms, response_out);
}

bool nsr_json_rpc_notify(nsr_json_rpc_t *rpc,
                         const char *method,
                         const nsr_json_buf_t *params)
{
    nsr_json_buf_t req;
    nsr_json_init(&req);
    nsr_json_obj_start(&req);
    nsr_json_key(&req, "jsonrpc");
    nsr_json_string(&req, "2.0");
    nsr_json_key(&req, "method");
    nsr_json_string(&req, method);
    nsr_json_key(&req, "params");
    if (params && params->len > 0)
        nsr_json_append_raw(&req, nsr_json_cstr(params), params->len);
    else
        nsr_json_obj_start(&req), nsr_json_obj_end(&req);
    nsr_json_obj_end(&req);

    bool ok = rpc_send(rpc, &req);
    nsr_json_free(&req);
    return ok;
}
