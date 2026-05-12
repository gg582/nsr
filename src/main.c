#define _GNU_SOURCE
#define _XOPEN_SOURCE 700
#include <nsr/omni.h>
#include <nsr/tui.h>
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

static pid_t g_gk_pid = 0;
static pid_t g_logic_pid = 0;
static bool g_running = true;

void signal_handler(int sig) {
    (void)sig;
    g_running = false;
}

void spawn_gatekeeper_omega(nsr_shm_ring_t *l2g, nsr_shm_ring_t *g2l, const char *target) {
    g_gk_pid = fork();
    if (g_gk_pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(0, &cpuset);
        if (sched_setaffinity(0, sizeof(cpuset), &cpuset) < 0) perror("affinity");
        
        nsr_omni_gatekeeper_omega(l2g, g2l, target);
        exit(0);
    }
}

#include <netdb.h>
#include <arpa/inet.h>

#include <getopt.h>

int main(int argc, char **argv) {
    uint32_t interval_ms = 100;
    bool silent = false;
    char *target_arg = NULL;

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
                fprintf(stderr, "Usage: %s <target> [-i ms] [--silent]\n", argv[0]);
                fprintf(stderr, "Options:\n");
                fprintf(stderr, "  -i, --interval MS   Transmission interval in milliseconds (default: 100)\n");
                fprintf(stderr, "  -s, --silent        Run without TUI\n");
                return EXIT_FAILURE;
        }
    }

    if (optind < argc) {
        target_arg = argv[optind];
    } else {
        fprintf(stderr, "Error: Target hostname or IP is required.\n");
        return EXIT_FAILURE;
    }

    // Resolve hostname if necessary
    char target_ip[64];
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_RAW;

    if (getaddrinfo(target_arg, NULL, &hints, &res) != 0) {
        fprintf(stderr, "Could not resolve hostname: %s\n", target_arg);
        return EXIT_FAILURE;
    }

    if (res->ai_family == AF_INET) {
        inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr, target_ip, sizeof(target_ip));
    } else if (res->ai_family == AF_INET6) {
        inet_ntop(AF_INET6, &((struct sockaddr_in6 *)res->ai_addr)->sin6_addr, target_ip, sizeof(target_ip));
    } else {
        fprintf(stderr, "Unsupported address family\n");
        freeaddrinfo(res);
        return EXIT_FAILURE;
    }
    freeaddrinfo(res);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGUSR1, SIG_IGN); // Supervisor ignores USR1

    size_t shm_size = sizeof(nsr_shm_ring_t);
    nsr_shm_ring_t *l2g = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, 
                               MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    nsr_shm_ring_t *g2l = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, 
                               MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    
    size_t shm_large_size = sizeof(nsr_shm_ring_large_t);
    nsr_shm_ring_large_t *l2t = mmap(NULL, shm_large_size, PROT_READ | PROT_WRITE, 
                                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    
    nsr_config_t *config = mmap(NULL, sizeof(nsr_config_t), PROT_READ | PROT_WRITE,
                                MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    
    if (l2g == MAP_FAILED || g2l == MAP_FAILED || l2t == MAP_FAILED || config == MAP_FAILED) {
        perror("mmap");
        return EXIT_FAILURE;
    }

    memset(l2g, 0, shm_size);
    memset(g2l, 0, shm_size);
    memset(l2t, 0, shm_large_size);
    atomic_init(&config->interval_ms, interval_ms);

    spawn_gatekeeper_omega(l2g, g2l, target_ip);
    
    // Updated spawn_logic_omega to take config (inline since it's small)
    g_logic_pid = fork();
    if (g_logic_pid == 0) {
        signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL);
        nsr_omni_logic_omega(g2l, l2g, l2t, config);
        exit(0);
    }

    if (!silent) {
        nsr_tui_init();
    }
    
    nsr_omni_state_t last_state;
    memset(&last_state, 0, sizeof(last_state));
    strncpy(last_state.target_ip, target_ip, sizeof(last_state.target_ip));

    while (g_running) {
        // ... (waitpid logic) ...
        int status;
        pid_t exited_pid = waitpid(-1, &status, WNOHANG);
        if (exited_pid > 0) {
            if (exited_pid == g_gk_pid) {
                if (!silent) fprintf(stderr, "[ULTRA] Gatekeeper crashed! status=%d\n", status);
                spawn_gatekeeper_omega(l2g, g2l, target_ip);
            } else if (exited_pid == g_logic_pid) {
                if (!silent) fprintf(stderr, "[ULTRA] Logic Engine crashed! status=%d\n", status);
                g_logic_pid = fork();
                if (g_logic_pid == 0) {
                    signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL);
                    nsr_omni_logic_omega(g2l, l2g, l2t, config);
                    exit(0);
                }
            }
        }

        nsr_omni_state_t new_state;
        while (nsr_shm_ring_large_pop(l2t, &new_state, sizeof(new_state))) {
            memcpy(&last_state, &new_state, sizeof(last_state));
        }
        
        if (!silent) {
            nsr_tui_render(&last_state);
            int cmd = nsr_tui_update();
            if (cmd == 1) break; // Quit
            if (cmd == 2) {
                if (g_logic_pid > 0) kill(g_logic_pid, SIGUSR1);
            }
            if (cmd == 4) { // Increase interval
                uint32_t current = atomic_load(&config->interval_ms);
                if (current < 1000) atomic_store(&config->interval_ms, current + 10);
            }
            if (cmd == 5) { // Decrease interval
                uint32_t current = atomic_load(&config->interval_ms);
                if (current > 10) {
                    atomic_store(&config->interval_ms, current - 10);
                } else if (current > 0) {
                     atomic_store(&config->interval_ms, 10);
                }
            }
            if (cmd == 6) { // Toggle Settings Dashboard
                nsr_tui_toggle_dashboard();
            }
        }
        
        struct timespec ts = {0, 16666666}; // 60fps check
        nanosleep(&ts, NULL);
    }

    if (!silent) nsr_tui_cleanup();
    if (g_gk_pid > 0) kill(g_gk_pid, SIGTERM);
    if (g_logic_pid > 0) kill(g_logic_pid, SIGTERM);
    waitpid(g_gk_pid, NULL, 0);
    waitpid(g_logic_pid, NULL, 0);

    return 0;
}
