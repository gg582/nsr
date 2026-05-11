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

void spawn_logic_omega(nsr_shm_ring_t *g2l, nsr_shm_ring_t *l2g, int l2t) {
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

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <target_ip> [--silent]\n", argv[0]);
        return EXIT_FAILURE;
    }

    bool silent = (argc > 2 && strcmp(argv[2], "--silent") == 0);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGUSR1, SIG_IGN); // Supervisor ignores USR1

    size_t shm_size = sizeof(nsr_shm_ring_t);
    nsr_shm_ring_t *l2g = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, 
                               MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    nsr_shm_ring_t *g2l = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, 
                               MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    
    if (l2g == MAP_FAILED || g2l == MAP_FAILED) {
        perror("mmap");
        return EXIT_FAILURE;
    }

    memset(l2g, 0, shm_size);
    memset(g2l, 0, shm_size);

    int l2t[2];
    if (pipe(l2t) < 0) {
        perror("pipe");
        return EXIT_FAILURE;
    }
    fcntl(l2t[1], F_SETPIPE_SZ, 1048576);
    fcntl(l2t[0], F_SETFL, O_NONBLOCK);

    spawn_gatekeeper_omega(l2g, g2l, argv[1]);
    spawn_logic_omega(g2l, l2g, l2t[1]);

    if (!silent) {
        nsr_tui_init();
    }
    
    nsr_omni_state_t last_state;
    memset(&last_state, 0, sizeof(last_state));
    strncpy(last_state.target_ip, argv[1], sizeof(last_state.target_ip));

    while (g_running) {
        int status;
        pid_t exited_pid = waitpid(-1, &status, WNOHANG);
        if (exited_pid > 0) {
            if (exited_pid == g_gk_pid) {
                if (!silent) fprintf(stderr, "[ULTRA] Gatekeeper crashed! status=%d\n", status);
                spawn_gatekeeper_omega(l2g, g2l, argv[1]);
            } else if (exited_pid == g_logic_pid) {
                if (!silent) fprintf(stderr, "[ULTRA] Logic Engine crashed! status=%d\n", status);
                spawn_logic_omega(g2l, l2g, l2t[1]);
            }
        }

        nsr_omni_state_t new_state;
        while (read(l2t[0], &new_state, sizeof(new_state)) == sizeof(new_state)) {
            memcpy(&last_state, &new_state, sizeof(last_state));
        }
        
        if (!silent) {
            nsr_tui_render(&last_state);
            if (nsr_tui_update()) break;
        }
        
        struct timespec ts = {0, 50000000}; 
        nanosleep(&ts, NULL);
    }

    if (!silent) nsr_tui_cleanup();
    if (g_gk_pid > 0) kill(g_gk_pid, SIGTERM);
    if (g_logic_pid > 0) kill(g_logic_pid, SIGTERM);
    waitpid(g_gk_pid, NULL, 0);
    waitpid(g_logic_pid, NULL, 0);

    return 0;
}
