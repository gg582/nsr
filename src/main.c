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

void spawn_logic_omega(nsr_shm_ring_t *g2l, nsr_shm_ring_t *l2g, nsr_shm_ring_large_t *l2t) {
    g_logic_pid = fork();
    if (g_logic_pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);

        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(2, &cpuset);
        if (sched_setaffinity(0, sizeof(cpuset), &cpuset) < 0) perror("affinity");

        nsr_omni_logic_omega(g2l, l2g, l2t);
        exit(0);
    }
}

#include <netdb.h>
#include <arpa/inet.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <target_ip_or_hostname> [--silent]\n", argv[0]);
        return EXIT_FAILURE;
    }

    bool silent = (argc > 2 && strcmp(argv[2], "--silent") == 0);

    // Resolve hostname if necessary
    char target_ip[64];
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_RAW;

    if (getaddrinfo(argv[1], NULL, &hints, &res) != 0) {
        fprintf(stderr, "Could not resolve hostname: %s\n", argv[1]);
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
    
    if (l2g == MAP_FAILED || g2l == MAP_FAILED || l2t == MAP_FAILED) {
        perror("mmap");
        return EXIT_FAILURE;
    }

    memset(l2g, 0, shm_size);
    memset(g2l, 0, shm_size);
    memset(l2t, 0, shm_large_size);

    spawn_gatekeeper_omega(l2g, g2l, target_ip);
    spawn_logic_omega(g2l, l2g, l2t);

    if (!silent) {
        nsr_tui_init();
    }
    
    nsr_omni_state_t last_state;
    memset(&last_state, 0, sizeof(last_state));
    strncpy(last_state.target_ip, target_ip, sizeof(last_state.target_ip));

    while (g_running) {
        int status;
        pid_t exited_pid = waitpid(-1, &status, WNOHANG);
        if (exited_pid > 0) {
            if (exited_pid == g_gk_pid) {
                if (!silent) fprintf(stderr, "[ULTRA] Gatekeeper crashed! status=%d\n", status);
                spawn_gatekeeper_omega(l2g, g2l, target_ip);
            } else if (exited_pid == g_logic_pid) {
                if (!silent) fprintf(stderr, "[ULTRA] Logic Engine crashed! status=%d\n", status);
                spawn_logic_omega(g2l, l2g, l2t);
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
                // Toggle pause in logic process
                if (g_logic_pid > 0) kill(g_logic_pid, SIGUSR1);
            }
            // cmd == 3 (Stats) is handled internally by TUI for display
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
