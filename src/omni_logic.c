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
#include <signal.h>
#include <errno.h>

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

void nsr_omni_logic_omega(nsr_shm_ring_t *g2l, nsr_shm_ring_t *l2g, int logic_to_tui_fd) {
    memset(&g_state, 0, sizeof(g_state));
    g_state.start_time_us = ttak_get_tick_count_ns() / 1000;
    
    uint8_t current_ttl = 1;
    uint16_t current_seq = 0;

    while (1) {
        // [1] EMIT PROBE INTENT (Batching to SHM)
        #define LOGIC_BATCH 32
        nsr_intent_t intents[LOGIC_BATCH];
        uint64_t now_us = ttak_get_tick_count_ns() / 1000;
        for (int i = 0; i < LOGIC_BATCH; i++) {
            intents[i].ttl = current_ttl;
            intents[i].seq = current_seq;
            intents[i].action = 0;
            intents[i].timestamp_us = now_us;
            intents[i].integrity = compute_integrity(current_ttl, current_seq);
            
            g_state.hops[current_ttl].sent++;
            current_seq++;
            
            current_ttl++;
            if (__builtin_expect(current_ttl > 30, 0)) current_ttl = 1;
        }
        
        while (!nsr_shm_ring_push_batch(l2g, intents, LOGIC_BATCH, sizeof(nsr_intent_t)));

        // [2] DRAIN OBSERVATIONS (SHM Pop)
        nsr_observation_t obs;
        while (nsr_shm_ring_pop(g2l, &obs, sizeof(obs))) {
            update_state(obs.ttl, &obs);
        }

        // [3] PERIODIC TUI UPDATE
        static uint64_t last_tui_us = 0;
        if (now_us - last_tui_us > 16666) {
            write(logic_to_tui_fd, &g_state, sizeof(g_state));
            last_tui_us = now_us;
        }
        
    }
}
