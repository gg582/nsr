/**
 * @file omni_logic.c
 * @brief High-Efficiency Tracer Logic Engine for NSR.
 */

#include <nsr/omni.h>
#include <ttak/timing/timing.h>
#include <ttak/security/siphash.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <poll.h>
#include <fcntl.h>

static nsr_omni_state_t g_state;

static void update_state(uint8_t ttl, nsr_observation_t *obs) {
    if (ttl >= NSR_MAX_HOPS || ttl == 0) return;
    nsr_hop_info_t *h = &g_state.hops[ttl];
    
    if (obs->type == NSR_OBS_REPLY || obs->type == NSR_OBS_EXCEEDED) {
        h->recv++;
        h->rtt_us = obs->rtt_us;
        strncpy(h->addr, obs->addr, sizeof(h->addr));
    }
    h->last_status = obs->type;
}

static uint64_t compute_integrity(uint8_t ttl, uint16_t seq) {
    uint64_t val = ((uint64_t)ttl << 16) | (uint64_t)seq;
    return ttak_siphash24_u64(val, NSR_INTEGRITY_KEY0, NSR_INTEGRITY_KEY1);
}

void nsr_omni_logic_main(int gate_to_logic_fd, int logic_to_gate_fd, int logic_to_tui_fd) {
    memset(&g_state, 0, sizeof(g_state));
    g_state.start_time_us = ttak_get_tick_count_ns() / 1000;
    
    uint8_t current_ttl = 1;
    uint16_t current_seq = 0;
    uint32_t probes_sent = 0;

    // Set non-blocking
    fcntl(gate_to_logic_fd, F_SETFL, O_NONBLOCK);

    struct pollfd fds[1];
    fds[0].fd = gate_to_logic_fd;
    fds[0].events = POLLIN;

    while (1) {
        // [1] EMIT PROBE INTENT
        nsr_intent_t intent = {
            .ttl = current_ttl,
            .seq = current_seq,
            .action = 0, // PROBE
            .timestamp_us = ttak_get_tick_count_ns() / 1000,
            .integrity = compute_integrity(current_ttl, current_seq)
        };
        
        g_state.hops[current_ttl].sent++;
        write(logic_to_gate_fd, &intent, sizeof(intent));
        
        current_seq++;
        probes_sent++;

        // [2] DRAIN OBSERVATIONS (Non-blocking)
        while (poll(fds, 1, 0) > 0) {
            nsr_observation_t obs;
            if (read(gate_to_logic_fd, &obs, sizeof(obs)) == sizeof(obs)) {
                update_state(obs.ttl, &obs);
            } else {
                break;
            }
        }

        // [3] PERIODIC TUI UPDATE
        if (probes_sent % 10 == 0) {
            write(logic_to_tui_fd, &g_state, sizeof(g_state));
        }

        // Round-robin TTL (1 to 30)
        current_ttl++;
        if (current_ttl > 30) current_ttl = 1;

        // Deterministic Pacing (10ms)
        struct timespec ts = {0, 10000000}; 
        nanosleep(&ts, NULL);
    }
}
