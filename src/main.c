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

static pid_t g_gk_pid = 0;
static pid_t g_logic_pid = 0;
static bool g_running = true;

void signal_handler(int sig) {
    (void)sig;
    g_running = false;
}

void spawn_gatekeeper(int l2g, int g2l, const char *target) {
    g_gk_pid = fork();
    if (g_gk_pid == 0) {
        // Reset signal handlers in child
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        nsr_omni_gatekeeper_main(l2g, g2l, target);
        exit(0);
    }
}

void spawn_logic(int g2l, int l2g, int l2t) {
    g_logic_pid = fork();
    if (g_logic_pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        nsr_omni_logic_main(g2l, l2g, l2t);
        exit(0);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <target_ip>\n", argv[0]);
        return EXIT_FAILURE;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    int l2g[2], g2l[2], l2t[2];
    if (pipe(l2g) < 0 || pipe(g2l) < 0 || pipe(l2t) < 0) {
        perror("pipe");
        return EXIT_FAILURE;
    }

    // Set TUI pipe to non-blocking
    fcntl(l2t[0], F_SETFL, O_NONBLOCK);

    // Initial Spawn
    spawn_gatekeeper(l2g[0], g2l[1], argv[1]);
    spawn_logic(g2l[0], l2g[1], l2t[1]);

    nsr_tui_init();
    
    nsr_omni_state_t last_state;
    memset(&last_state, 0, sizeof(last_state));
    strncpy(last_state.target_ip, argv[1], sizeof(last_state.target_ip));

    printf("[ULTRA] Supervisor active. Monitoring Gatekeeper(%d) and Logic(%d)\n", g_gk_pid, g_logic_pid);

    while (g_running) {
        // [1] Monitor Children
        int status;
        pid_t exited_pid = waitpid(-1, &status, WNOHANG);
        if (exited_pid > 0) {
            if (exited_pid == g_gk_pid) {
                fprintf(stderr, "[ULTRA] Gatekeeper crashed! Restarting...\n");
                spawn_gatekeeper(l2g[0], g2l[1], argv[1]);
            } else if (exited_pid == g_logic_pid) {
                fprintf(stderr, "[ULTRA] Logic Engine crashed! Restarting...\n");
                spawn_logic(g2l[0], l2g[1], l2t[1]);
            }
        }

        // [2] Handle TUI Data
        nsr_omni_state_t new_state;
        while (read(l2t[0], &new_state, sizeof(new_state)) == sizeof(new_state)) {
            memcpy(&last_state, &new_state, sizeof(last_state));
        }
        nsr_tui_render(&last_state);

        if (nsr_tui_update()) break;
        
        struct timespec ts = {0, 50000000}; // 50ms check loop
        nanosleep(&ts, NULL);
    }

    nsr_tui_cleanup();
    
    if (g_gk_pid > 0) kill(g_gk_pid, SIGTERM);
    if (g_logic_pid > 0) kill(g_logic_pid, SIGTERM);
    
    waitpid(g_gk_pid, NULL, 0);
    waitpid(g_logic_pid, NULL, 0);

    return 0;
}
