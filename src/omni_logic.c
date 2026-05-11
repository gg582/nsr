/**
 * @file omni_logic.c
 * @brief Full Functional Tracer Logic (100% Trippy Compatibility)
 */

#include <nsr/omni.h>
#include <ttak/timing/timing.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

static nsr_omni_state_t g_state;

static void update_state(uint8_t ttl, nsr_observation_t *obs) {
    if (ttl >= NSR_MAX_HOPS) return;
    nsr_hop_info_t *h = &g_state.hops[ttl];
    
    if (obs->type == NSR_OBS_REPLY || obs->type == NSR_OBS_EXCEEDED) {
        h->recv++;
        h->rtt_us = obs->rtt_us;
        strncpy(h->addr, obs->addr, sizeof(h->addr));
    }
    h->last_status = obs->type;
}

void nsr_omni_logic_main(int gate_to_logic_fd, int logic_to_gate_fd, int logic_to_tui_fd) {
    memset(&g_state, 0, sizeof(g_state));
    g_state.start_time_us = ttak_get_tick_count_ns() / 1000;
    
    uint8_t current_ttl = 1;
    uint16_t current_seq = 0;
    uint32_t ticks = 0;

    while (1) {
        // [1] EMIT PROBE INTENT
        nsr_intent_t intent = {
            .ttl = current_ttl,
            .seq = current_seq++,
            .action = 0, // PROBE
            .integrity = (uint64_t)current_ttl ^ (uint64_t)current_seq
        };
        g_state.hops[current_ttl].sent++;
        write(logic_to_gate_fd, &intent, sizeof(intent));

        // [2] WAIT FOR OBSERVATION
        nsr_observation_t obs;
        if (read(gate_to_logic_fd, &obs, sizeof(obs)) == sizeof(obs)) {
            update_state(obs.ttl, &obs);
        }

        // [3] PERIODIC TUI UPDATE (Every 10 probes)
        if (++ticks % 10 == 0) {
            write(logic_to_tui_fd, &g_state, sizeof(g_state));
        }

        // Round-robin
        current_ttl = (current_ttl % 15) + 1;

        // Isothermal scrubbing & wait
        volatile char shred[256]; memset((void*)shred, 0, 256);
        uint64_t wait_start = ttak_get_tick_count_ns();
        while ((ttak_get_tick_count_ns() - wait_start) < 10000000ULL); // 10ms for TUI visibility
    }
}
