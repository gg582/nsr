#include <nsr/singular.h>
#include <ttak/timing/timing.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <poll.h>

void nsr_singular_broker_main(const char *target) {
    int s_pipe[2]; // Broker -> Sender
    int r_pipe[2]; // Receiver -> Broker
    
    if (pipe(s_pipe) < 0 || pipe(r_pipe) < 0) {
        perror("pipe");
        return;
    }

    pid_t s_pid = fork();
    if (s_pid == 0) {
        close(s_pipe[1]); close(r_pipe[0]); close(r_pipe[1]);
        nsr_singular_sender_main(s_pipe[0]);
        exit(0);
    }

    pid_t r_pid = fork();
    if (r_pid == 0) {
        close(s_pipe[0]); close(s_pipe[1]); close(r_pipe[0]);
        nsr_singular_receiver_main(r_pipe[1]);
        exit(0);
    }

    close(s_pipe[0]); close(r_pipe[1]);

    printf("[SINGULAR] Broker active. Target: %s\n", target);

    struct pollfd fds[1];
    fds[0].fd = r_pipe[0];
    fds[0].events = POLLIN;

    uint16_t seq = 0;
    while (1) {
        // 1. Send Probe Command
        nsr_msg_t cmd = {
            .magic = NSR_MSG_MAGIC,
            .type = 0, // SEND_REQ
            .ttl = (uint8_t)(seq % 30 + 1),
            .seq = seq++,
            .ts_us = ttak_get_tick_count_ns() / 1000
        };
        write(s_pipe[1], &cmd, sizeof(cmd));

        // 2. Poll for Events
        if (poll(fds, 1, 10) > 0) {
            nsr_msg_t event;
            if (read(r_pipe[0], &event, sizeof(event)) == sizeof(event)) {
                if (event.magic == NSR_MSG_MAGIC && event.type == 1 /* RECV_EVT */) {
                    uint64_t rtt = (ttak_get_tick_count_ns() / 1000) - event.ts_us;
                    printf("[SINGULAR] RTT: %lu us, Seq: %u, TTL: %d\n", rtt, event.seq, event.ttl);
                }
            }
        }

        struct timespec ts = {0, 100000000}; // 100ms pacing
        nanosleep(&ts, NULL);
    }
}
