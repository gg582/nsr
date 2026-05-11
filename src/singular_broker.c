#include <nsr/singular.h>
#include <ttak/process/supervisor.h>
#include <ttak/io/multiplex.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

uint64_t g_nsr_mask = 0xDEADC0DECAFEBABEULL; // Should be initialized with HW RNG

/**
 * @brief Immutable Event Log (Append-Only)
 */
#define MAX_LOG_ENTRIES 10000
static nsr_msg_t g_event_log[MAX_LOG_ENTRIES];
static size_t g_log_idx = 0;

static void append_to_log(const nsr_msg_t *msg) {
    // Immutable append logic
    if (g_log_idx < MAX_LOG_ENTRIES) {
        g_event_log[g_log_idx++] = *msg;
    }
}

void nsr_singular_broker_main(const char *target) {
    int sender_pipe[2];   // Broker -> Sender
    int receiver_pipe[2]; // Receiver -> Broker
    
    pipe(sender_pipe);
    pipe(receiver_pipe);

    // 1. Spawn Workers (Fault-Tolerant)
    ttak_supervisor_t *sv = ttak_supervisor_create();
    ttak_supervisor_spawn_ext(sv, (void*)nsr_singular_sender_main, sender_pipe[0]);
    ttak_supervisor_spawn_ext(sv, (void*)nsr_singular_receiver_main, receiver_pipe[1]);

    printf("[SINGULAR] Broker active. target=%s\n", target);

    while (true) {
        // Zero-Shared Communication: Only messaging, no SHM
        ttak_io_wait_t wait;
        ttak_io_multiplex_wait(&wait, receiver_pipe[0], 500 /* ms */);

        if (wait.triggered) {
            nsr_msg_t event;
            if (read(receiver_pipe[0], &event, sizeof(event)) == sizeof(event)) {
                if (event.magic == NSR_MSG_MAGIC) {
                    append_to_log(&event);
                }
            }
        }

        // Periodic Tick: Command Sender to fire new probes
        nsr_msg_t cmd = {
            .magic = NSR_MSG_MAGIC,
            .type = 0, // SEND_REQ
            .ttl = (uint8_t)(rand() % 15 + 1),
            .ts_us = ttak_timing_now_us()
        };
        write(sender_pipe[1], &cmd, sizeof(cmd));

        // Reconstruct TUI view from Immutable Log (Simplified)
        // nsr_tui_render_from_log(g_event_log, g_log_idx);
        
        ttak_timing_sleep(ttak_duration_from_ms(100));
    }
}
