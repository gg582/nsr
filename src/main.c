#define _XOPEN_SOURCE 700
#include <nsr/omni.h>
#include <nsr/tui.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <target_ip>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int l2g[2], g2l[2], l2t[2];
    if (pipe(l2g) < 0 || pipe(g2l) < 0 || pipe(l2t) < 0) {
        perror("pipe");
        return EXIT_FAILURE;
    }

    // [1] Spawn Gatekeeper
    pid_t gk_pid = fork();
    if (gk_pid == 0) {
        close(l2g[1]); close(g2l[0]); close(l2t[0]); close(l2t[1]);
        nsr_omni_gatekeeper_main(l2g[0], g2l[1], argv[1]);
        exit(0);
    }

    // [2] Spawn Logic Engine
    pid_t logic_pid = fork();
    if (logic_pid == 0) {
        close(l2g[0]); close(g2l[1]); close(l2t[0]);
        nsr_omni_logic_main(g2l[0], l2g[1], l2t[1]);
        exit(0);
    }

    // [3] Parent: TUI Orchestrator
    close(l2g[0]); close(l2g[1]); close(g2l[0]); close(g2l[1]); close(l2t[1]);
    
    // Set TUI pipe to non-blocking
    int flags = fcntl(l2t[0], F_GETFL, 0);
    fcntl(l2t[0], F_SETFL, flags | O_NONBLOCK);

    nsr_tui_init();
    
    nsr_omni_state_t last_state;
    memset(&last_state, 0, sizeof(last_state));
    strncpy(last_state.target_ip, argv[1], sizeof(last_state.target_ip));

    while (1) {
        nsr_omni_state_t new_state;
        if (read(l2t[0], &new_state, sizeof(new_state)) == sizeof(new_state)) {
            memcpy(&last_state, &new_state, sizeof(last_state));
            nsr_tui_render(&last_state);
        }

        if (nsr_tui_update()) break;
        
        struct timespec ts = {0, 10000000}; // 10ms
        nanosleep(&ts, NULL);
    }

    nsr_tui_cleanup();
    kill(gk_pid, SIGTERM);
    kill(logic_pid, SIGTERM);
    waitpid(gk_pid, NULL, 0);
    waitpid(logic_pid, NULL, 0);

    return 0;
}
