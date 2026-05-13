#define _GNU_SOURCE
#define _XOPEN_SOURCE 700
#include <nsr/telemetry.h>
#include <nsr/tui.h>
#include <nsr/topology.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <sched.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <getopt.h>

#define NSR_MAX_TARGETS 8

typedef struct {
    nsr_shm_ring_t *l2g;
    nsr_shm_ring_t *g2l;
    nsr_shm_ring_large_t *l2t;
    pid_t gk_pid;
    pid_t logic_pid;
    char target_ip[64];
    bool active;
} nsr_session_t;

static nsr_session_t g_sessions[NSR_MAX_TARGETS];
static int g_num_sessions = 0;
static bool g_running = true;

static void signal_handler(int sig)
{
    (void)sig;
    g_running = false;
}

static void spawn_gatekeeper(nsr_session_t *sess)
{
    sess->gk_pid = fork();
    if (sess->gk_pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(0, &cpuset);
        if (sched_setaffinity(0, sizeof(cpuset), &cpuset) < 0)
            perror("affinity");
        nsr_gatekeeper_run(sess->l2g, sess->g2l, sess->target_ip);
        exit(0);
    }
}

static bool resolve_target(const char *arg, char *out_ip, size_t out_len)
{
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_RAW;

    if (getaddrinfo(arg, NULL, &hints, &res) != 0)
        return false;

    if (res->ai_family == AF_INET) {
        inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr, out_ip, (socklen_t)out_len);
    } else if (res->ai_family == AF_INET6) {
        inet_ntop(AF_INET6, &((struct sockaddr_in6 *)res->ai_addr)->sin6_addr, out_ip, (socklen_t)out_len);
    } else {
        freeaddrinfo(res);
        return false;
    }
    freeaddrinfo(res);
    return true;
}

int main(int argc, char **argv)
{
    uint32_t interval_ms = 100;
    bool silent = false;

    static struct option long_options[] = {
        {"interval", required_argument, 0, 'i'},
        {"silent",   no_argument,       0, 's'},
        {"help",     no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "i:sh", long_options, NULL)) != -1) {
        switch (opt) {
        case 'i':
            interval_ms = atoi(optarg);
            if (interval_ms < 10) {
                fprintf(stderr, "Notice: Adjusted interval from %u ms to 10 ms to properly diagnose.\n", interval_ms);
                interval_ms = 10;
            } else if (interval_ms > 1000) {
                fprintf(stderr, "Notice: Adjusted interval from %u ms to 1000 ms to properly diagnose.\n", interval_ms);
                interval_ms = 1000;
            }
            break;
        case 's':
            silent = true;
            break;
        case 'h':
        default:
            fprintf(stderr, "Usage: %s <target> [<target> ...] [-i ms] [--silent]\n", argv[0]);
            fprintf(stderr, "Options:\n");
            fprintf(stderr, "  -i, --interval MS   Transmission interval in milliseconds (default: 100)\n");
            fprintf(stderr, "  -s, --silent        Run without TUI\n");
            return EXIT_FAILURE;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "Error: At least one target hostname or IP is required.\n");
        return EXIT_FAILURE;
    }

    for (int i = optind; i < argc && g_num_sessions < NSR_MAX_TARGETS; i++) {
        char ip[64];
        if (!resolve_target(argv[i], ip, sizeof(ip))) {
            fprintf(stderr, "Could not resolve hostname: %s\n", argv[i]);
            continue;
        }

        nsr_session_t *sess = &g_sessions[g_num_sessions];
        memset(sess, 0, sizeof(*sess));
        strncpy(sess->target_ip, ip, sizeof(sess->target_ip) - 1);
        sess->active = true;

        size_t shm_size = sizeof(nsr_shm_ring_t);
        sess->l2g = mmap(NULL, shm_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        sess->g2l = mmap(NULL, shm_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        size_t shm_large_size = sizeof(nsr_shm_ring_large_t);
        sess->l2t = mmap(NULL, shm_large_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);

        if (sess->l2g == MAP_FAILED || sess->g2l == MAP_FAILED || sess->l2t == MAP_FAILED) {
            perror("mmap");
            return EXIT_FAILURE;
        }

        memset(sess->l2g, 0, shm_size);
        memset(sess->g2l, 0, shm_size);
        memset(sess->l2t, 0, shm_large_size);

        spawn_gatekeeper(sess);

        sess->logic_pid = fork();
        if (sess->logic_pid == 0) {
            signal(SIGINT, SIG_DFL);
            signal(SIGTERM, SIG_DFL);
            nsr_config_t *config = mmap(NULL, sizeof(nsr_config_t), PROT_READ | PROT_WRITE,
                                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
            atomic_init(&config->interval_ms, interval_ms);
            strncpy(config->target_ip, sess->target_ip, sizeof(config->target_ip) - 1);
            nsr_logic_run(sess->g2l, sess->l2g, sess->l2t, config);
            exit(0);
        }

        g_num_sessions++;
    }

    if (g_num_sessions == 0) {
        fprintf(stderr, "Error: No valid targets.\n");
        return EXIT_FAILURE;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGUSR1, SIG_IGN);

    nsr_topology_state_t topology;
    nsr_topology_init(&topology);

    nsr_tui_state_t tui_state;
    memset(&tui_state, 0, sizeof(tui_state));
    tui_state.current_mode = NSR_UI_NORMAL;

    if (!silent) {
        nsr_tui_init();
    }

    nsr_telemetry_state_t last_state;
    memset(&last_state, 0, sizeof(last_state));

    while (g_running) {
        int status;
        pid_t exited_pid = waitpid(-1, &status, WNOHANG);
        if (exited_pid > 0) {
            for (int s = 0; s < g_num_sessions; s++) {
                nsr_session_t *sess = &g_sessions[s];
                if (exited_pid == sess->gk_pid) {
                    if (!silent)
                        fprintf(stderr, "[GATEKEEPER] Process crashed! target=%s status=%d\n",
                                sess->target_ip, status);
                    spawn_gatekeeper(sess);
                } else if (exited_pid == sess->logic_pid) {
                    if (!silent)
                        fprintf(stderr, "[LOGIC] Process crashed! target=%s status=%d\n",
                                sess->target_ip, status);
                    sess->logic_pid = fork();
                    if (sess->logic_pid == 0) {
                        signal(SIGINT, SIG_DFL);
                        signal(SIGTERM, SIG_DFL);
                        nsr_config_t *config = mmap(NULL, sizeof(nsr_config_t), PROT_READ | PROT_WRITE,
                                                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
                        atomic_init(&config->interval_ms, interval_ms);
                        strncpy(config->target_ip, sess->target_ip, sizeof(config->target_ip) - 1);
                        nsr_logic_run(sess->g2l, sess->l2g, sess->l2t, config);
                        exit(0);
                    }
                }
            }
        }

        for (int s = 0; s < g_num_sessions; s++) {
            nsr_session_t *sess = &g_sessions[s];
            nsr_telemetry_state_t new_state;
            while (nsr_shm_ring_large_pop(sess->l2t, &new_state, sizeof(new_state))) {
                memcpy(&last_state, &new_state, sizeof(last_state));
                nsr_topology_update_from_telemetry(&topology, &last_state);
            }
        }

        if (!silent) {
            nsr_tui_render(&tui_state, &last_state, &topology);
            int cmd = nsr_tui_update(&tui_state, &topology);
            if (cmd == 1)
                break;
            if (cmd == 2) {
                for (int s = 0; s < g_num_sessions; s++) {
                    if (g_sessions[s].logic_pid > 0)
                        kill(g_sessions[s].logic_pid, SIGUSR1);
                }
            }
            if (cmd == 4) {
                if (interval_ms < 1000)
                    interval_ms += 10;
            }
            if (cmd == 5) {
                if (interval_ms > 10) {
                    interval_ms -= 10;
                } else if (interval_ms > 0) {
                    interval_ms = 10;
                }
            }
            if (cmd == 6) {
                nsr_tui_toggle_dashboard(&tui_state);
            }
        }

        struct timespec ts = {0, 16666666};
        nanosleep(&ts, NULL);
    }

    if (!silent)
        nsr_tui_cleanup();

    for (int s = 0; s < g_num_sessions; s++) {
        nsr_session_t *sess = &g_sessions[s];
        if (sess->gk_pid > 0)
            kill(sess->gk_pid, SIGTERM);
        if (sess->logic_pid > 0)
            kill(sess->logic_pid, SIGTERM);
    }
    for (int s = 0; s < g_num_sessions; s++) {
        nsr_session_t *sess = &g_sessions[s];
        if (sess->gk_pid > 0)
            waitpid(sess->gk_pid, NULL, 0);
        if (sess->logic_pid > 0)
            waitpid(sess->logic_pid, NULL, 0);
    }

    return 0;
}
