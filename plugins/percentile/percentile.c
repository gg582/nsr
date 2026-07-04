#include <nsr/json/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <limits.h>

#define PCTL_SAMPLE_MAX 4096
#define PCTL_ADDR_MAX   48
#define PCTL_BUCKET_MAX 60  /* 1 hour in 1-minute buckets */

typedef struct {
    uint64_t rtt_us;
    uint64_t ts_ms;
    char addr[PCTL_ADDR_MAX];
} pctl_sample_t;

typedef struct {
    pctl_sample_t samples[PCTL_SAMPLE_MAX];
    uint32_t head;
    uint32_t count;
} pctl_state_t;

typedef struct {
    uint64_t start_ms;
    uint64_t min_us;
    uint64_t max_us;
    uint64_t sum_us;
    uint32_t count;
} pctl_bucket_t;

typedef struct {
    pctl_bucket_t buckets[PCTL_BUCKET_MAX];
    uint32_t head;
    uint32_t count;
} pctl_bucket_ring_t;

typedef struct {
    uint64_t min_us;
    uint64_t max_us;
    uint64_t avg_us;
    bool valid;
} pctl_stats_t;

typedef struct {
    const char *label;
    uint32_t buckets;
    uint64_t window_ms;
} pctl_window_t;

static pctl_state_t g_samples;
static pctl_bucket_ring_t g_buckets;

static const pctl_window_t WINDOWS[] = {
    {"1min",  1,  60 * 1000},
    {"5min",  5,  5 * 60 * 1000},
    {"15min", 15, 15 * 60 * 1000},
    {"1hr",   60, 60 * 60 * 1000},
};

static uint64_t now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static uint64_t bucket_start_ms(uint64_t ts_ms)
{
    return (ts_ms / 60000ULL) * 60000ULL;
}

static void pctl_add(const char *addr, uint64_t rtt_us, uint64_t ts_ms)
{
    if (!addr || !addr[0] || rtt_us == 0)
        return;
    pctl_sample_t *s = &g_samples.samples[g_samples.head];
    memset(s->addr, 0, sizeof(s->addr));
    memcpy(s->addr, addr, sizeof(s->addr) - 1);
    s->rtt_us = rtt_us;
    s->ts_ms = ts_ms;
    g_samples.head = (g_samples.head + 1) % PCTL_SAMPLE_MAX;
    if (g_samples.count < PCTL_SAMPLE_MAX)
        g_samples.count++;
}

static pctl_bucket_t *get_current_bucket(uint64_t ts_ms)
{
    uint64_t start = bucket_start_ms(ts_ms);

    for (uint32_t i = 0; i < g_buckets.count; i++) {
        uint32_t idx = (g_buckets.head + PCTL_BUCKET_MAX - g_buckets.count + i) % PCTL_BUCKET_MAX;
        if (g_buckets.buckets[idx].start_ms == start)
            return &g_buckets.buckets[idx];
    }

    uint32_t idx = g_buckets.head;
    g_buckets.buckets[idx].start_ms = start;
    g_buckets.buckets[idx].min_us = UINT64_MAX;
    g_buckets.buckets[idx].max_us = 0;
    g_buckets.buckets[idx].sum_us = 0;
    g_buckets.buckets[idx].count = 0;
    g_buckets.head = (g_buckets.head + 1) % PCTL_BUCKET_MAX;
    if (g_buckets.count < PCTL_BUCKET_MAX)
        g_buckets.count++;

    return &g_buckets.buckets[idx];
}

static void bucket_add(uint64_t ts_ms, uint64_t rtt_us)
{
    pctl_bucket_t *b = get_current_bucket(ts_ms);
    if (rtt_us < b->min_us)
        b->min_us = rtt_us;
    if (rtt_us > b->max_us)
        b->max_us = rtt_us;
    b->sum_us += rtt_us;
    b->count++;
}

static pctl_stats_t compute_window_stats(uint32_t num_buckets)
{
    pctl_stats_t s = {0, 0, 0, false};
    if (g_buckets.count == 0 || num_buckets == 0)
        return s;

    uint32_t n = num_buckets < g_buckets.count ? num_buckets : g_buckets.count;
    uint64_t min_us = UINT64_MAX;
    uint64_t max_us = 0;
    uint64_t sum_us = 0;
    uint64_t count = 0;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = (g_buckets.head + PCTL_BUCKET_MAX - n + i) % PCTL_BUCKET_MAX;
        pctl_bucket_t *b = &g_buckets.buckets[idx];
        if (b->count == 0)
            continue;
        if (b->min_us < min_us)
            min_us = b->min_us;
        if (b->max_us > max_us)
            max_us = b->max_us;
        sum_us += b->sum_us;
        count += b->count;
    }

    if (count == 0)
        return s;

    s.min_us = min_us;
    s.max_us = max_us;
    s.avg_us = sum_us / count;
    s.valid = true;
    return s;
}

static pctl_stats_t compute_window_stats_for_addr(const char *addr, uint64_t window_ms)
{
    pctl_stats_t s = {0, 0, 0, false};
    if (!addr || !addr[0] || g_samples.count == 0)
        return s;

    uint64_t now = now_ms();
    uint64_t cutoff = now - window_ms;
    uint64_t min_us = UINT64_MAX;
    uint64_t max_us = 0;
    uint64_t sum_us = 0;
    uint32_t count = 0;

    for (uint32_t i = 0; i < g_samples.count; i++) {
        uint32_t idx = (g_samples.head + PCTL_SAMPLE_MAX - g_samples.count + i) % PCTL_SAMPLE_MAX;
        pctl_sample_t *sample = &g_samples.samples[idx];
        if (strcmp(sample->addr, addr) != 0)
            continue;
        if (sample->ts_ms < cutoff)
            continue;
        if (sample->rtt_us < min_us)
            min_us = sample->rtt_us;
        if (sample->rtt_us > max_us)
            max_us = sample->rtt_us;
        sum_us += sample->rtt_us;
        count++;
    }

    if (count == 0)
        return s;

    s.min_us = min_us;
    s.max_us = max_us;
    s.avg_us = sum_us / count;
    s.valid = true;
    return s;
}

static void emit_title(nsr_json_buf_t *jb, const char *title,
                       int *line_y, int start_x)
{
    nsr_json_obj_start(jb);
    nsr_json_key(jb, "y");
    nsr_json_int(jb, (*line_y)++);
    nsr_json_key(jb, "x");
    nsr_json_int(jb, start_x);
    nsr_json_key(jb, "text");
    nsr_json_string(jb, title);
    nsr_json_key(jb, "color");
    nsr_json_string(jb, "cyan");
    nsr_json_obj_end(jb);
}

static void emit_stats_line(nsr_json_buf_t *jb, const char *label,
                            const pctl_stats_t *s,
                            int *line_y, int start_x)
{
    char buf[80];
    if (s->valid) {
        snprintf(buf, sizeof(buf), "%-5s min/avg/max  %6.2f/%6.2f/%6.2f ms",
                 label,
                 s->min_us / 1000.0,
                 s->avg_us / 1000.0,
                 s->max_us / 1000.0);
    } else {
        snprintf(buf, sizeof(buf), "%-5s min/avg/max  %6s/%6s/%6s ms",
                 label, "N/A", "N/A", "N/A");
    }

    nsr_json_obj_start(jb);
    nsr_json_key(jb, "y");
    nsr_json_int(jb, (*line_y)++);
    nsr_json_key(jb, "x");
    nsr_json_int(jb, start_x);
    nsr_json_key(jb, "text");
    nsr_json_string(jb, buf);
    nsr_json_obj_end(jb);
}

static void emit_window_stats(nsr_json_buf_t *jb, const char *title,
                              int *line_y, int start_x,
                              bool use_buckets,
                              const char *focus_addr)
{
    emit_title(jb, title, line_y, start_x);

    for (size_t i = 0; i < sizeof(WINDOWS) / sizeof(WINDOWS[0]); i++) {
        pctl_stats_t s;
        if (use_buckets) {
            s = compute_window_stats(WINDOWS[i].buckets);
        } else {
            s = compute_window_stats_for_addr(focus_addr, WINDOWS[i].window_ms);
        }
        emit_stats_line(jb, WINDOWS[i].label, &s, line_y, start_x);
    }
}

static void handle_init(long long id)
{
    nsr_json_buf_t resp;
    nsr_json_init(&resp);
    nsr_json_obj_start(&resp);
    nsr_json_key(&resp, "jsonrpc");
    nsr_json_string(&resp, "2.0");
    nsr_json_key(&resp, "id");
    nsr_json_int(&resp, id);
    nsr_json_key(&resp, "result");
    nsr_json_obj_start(&resp);
    nsr_json_key(&resp, "status");
    nsr_json_string(&resp, "ok");
    nsr_json_key(&resp, "description");
    nsr_json_string(&resp, "Show RTT min/avg/max for 1m/5m/15m/1h windows");
    nsr_json_obj_end(&resp);
    nsr_json_obj_end(&resp);
    printf("%s\n", nsr_json_cstr(&resp));
    fflush(stdout);
    nsr_json_free(&resp);
}

static void handle_update_telemetry(const char *params)
{
    size_t len;
    const char *hops = nsr_json_obj_get(params, "hops", &len);
    if (!hops)
        return;

    uint64_t ts = now_ms();

    const char *next;
    const char *elem = nsr_json_arr_first(hops, &next);
    while (elem) {
        char addr[64] = "";
        long long rtt_us = 0;
        long long recv = 0;
        size_t flen;
        const char *v;
        v = nsr_json_obj_get(elem, "addr", &flen);
        if (v) nsr_json_parse_str(v, flen, addr, sizeof(addr));
        v = nsr_json_obj_get(elem, "rtt_us", &flen);
        if (v) nsr_json_parse_int(v, flen, &rtt_us);
        v = nsr_json_obj_get(elem, "recv", &flen);
        if (v) nsr_json_parse_int(v, flen, &recv);
        for (long long k = 0; k < recv && k < 64; k++) {
            pctl_add(addr, (uint64_t)rtt_us, ts);
            bucket_add(ts, (uint64_t)rtt_us);
        }
        elem = nsr_json_arr_next(next, &next);
    }
}

static void handle_render(const char *params, long long id)
{
    char focus_addr[64] = "";
    size_t flen;
    const char *v = nsr_json_obj_get(params, "focused_addr", &flen);
    if (v) nsr_json_parse_str(v, flen, focus_addr, sizeof(focus_addr));

    nsr_json_buf_t resp;
    nsr_json_init(&resp);
    nsr_json_obj_start(&resp);
    nsr_json_key(&resp, "jsonrpc");
    nsr_json_string(&resp, "2.0");
    nsr_json_key(&resp, "id");
    nsr_json_int(&resp, id);
    nsr_json_key(&resp, "result");
    nsr_json_obj_start(&resp);
    nsr_json_key(&resp, "lines");
    nsr_json_arr_start(&resp);

    int y = 1;
    emit_window_stats(&resp, "RTT Statistics (Global)", &y, 1, true, nullptr);

    if (focus_addr[0]) {
        y++;
        emit_window_stats(&resp, "RTT Statistics (Focused)", &y, 1, false, focus_addr);
    }

    nsr_json_arr_end(&resp);
    nsr_json_obj_end(&resp);
    nsr_json_obj_end(&resp);
    printf("%s\n", nsr_json_cstr(&resp));
    fflush(stdout);
    nsr_json_free(&resp);
}

static void send_error(long long id, const char *message)
{
    nsr_json_buf_t resp;
    nsr_json_init(&resp);
    nsr_json_obj_start(&resp);
    nsr_json_key(&resp, "jsonrpc");
    nsr_json_string(&resp, "2.0");
    nsr_json_key(&resp, "id");
    nsr_json_int(&resp, id);
    nsr_json_key(&resp, "error");
    nsr_json_obj_start(&resp);
    nsr_json_key(&resp, "code");
    nsr_json_int(&resp, -32601);
    nsr_json_key(&resp, "message");
    nsr_json_string(&resp, message);
    nsr_json_obj_end(&resp);
    nsr_json_obj_end(&resp);
    printf("%s\n", nsr_json_cstr(&resp));
    fflush(stdout);
    nsr_json_free(&resp);
}

int main(void)
{
    memset(&g_samples, 0, sizeof(g_samples));
    memset(&g_buckets, 0, sizeof(g_buckets));

    char *line = nullptr;
    size_t cap = 0;
    while (getline(&line, &cap, stdin) != -1) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[--len] = '\0';
        if (len > 0 && line[len - 1] == '\r')
            line[--len] = '\0';

        size_t mlen;
        const char *method = nsr_json_obj_get(line, "method", &mlen);
        if (!method)
            continue;

        size_t plen;
        const char *params = nsr_json_obj_get(line, "params", &plen);
        if (!params)
            params = "{}";

        long long id = 0;
        bool has_id = false;
        size_t ilen;
        const char *idv = nsr_json_obj_get(line, "id", &ilen);
        if (idv) {
            has_id = true;
            nsr_json_parse_int(idv, ilen, &id);
        }

        char method_str[64];
        if (!nsr_json_parse_str(method, mlen, method_str, sizeof(method_str)))
            continue;

        if (strcmp(method_str, "init") == 0) {
            if (has_id) handle_init(id);
        } else if (strcmp(method_str, "update_telemetry") == 0) {
            handle_update_telemetry(params);
        } else if (strcmp(method_str, "render") == 0) {
            if (has_id) handle_render(params, id);
        } else if (strcmp(method_str, "cleanup") == 0) {
            /* no-op */
        } else if (has_id) {
            send_error(id, "Method not found");
        }
    }

    free(line);
    return 0;
}
