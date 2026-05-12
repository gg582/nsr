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
#include <arpa/inet.h>

typedef struct {
    uint8_t addr_bin[16];
    bool is_v6;
} nsr_hop_addr_cache_t;

static nsr_omni_state_t g_state;
static nsr_hop_addr_cache_t g_addr_cache[NSR_MAX_HOPS];
static volatile bool g_paused = false;

static void handle_sigusr1(int sig) {
    (void)sig;
    g_paused = !g_paused;
}

static void update_state(uint8_t ttl, nsr_observation_t *obs) {
    if (ttl >= NSR_MAX_HOPS || ttl == 0) return;
    nsr_hop_info_t *h = &g_state.hops[ttl];
    
    if (obs->type == NSR_OBS_REPLY || obs->type == NSR_OBS_EXCEEDED) {
        h->recv++;
        h->rtt_us = obs->rtt_us;
        
        bool changed = (obs->is_v6 != g_addr_cache[ttl].is_v6) || 
                       (memcmp(g_addr_cache[ttl].addr_bin, obs->addr_bin, obs->is_v6 ? 16 : 4) != 0);
        
        if (__builtin_expect(changed, 0)) {
            g_addr_cache[ttl].is_v6 = obs->is_v6;
            memcpy(g_addr_cache[ttl].addr_bin, obs->addr_bin, 16);
            if (obs->is_v6) {
                inet_ntop(AF_INET6, obs->addr_bin, h->addr, sizeof(h->addr));
            } else {
                struct in_addr ia; memcpy(&ia, obs->addr_bin, 4);
                inet_ntop(AF_INET, &ia, h->addr, sizeof(h->addr));
            }
        }
    }
    h->last_status = obs->type;
}

static inline uint64_t compute_integrity_fast(uint8_t ttl, uint16_t seq) {
    return ((uint64_t)ttl << 16) | (uint64_t)seq;
}

void nsr_omni_logic_omega(nsr_shm_ring_t *g2l, nsr_shm_ring_t *l2g, nsr_shm_ring_large_t *l2t, nsr_config_t *config) {
    signal(SIGUSR1, handle_sigusr1);
    memset(&g_state, 0, sizeof(g_state));
    memset(g_addr_cache, 0, sizeof(g_addr_cache));
    g_state.start_time_us = ttak_get_tick_count_ns() / 1000;
    
    uint8_t current_ttl = 1;
    uint16_t current_seq = 0;
    uint64_t last_probe_us = 0;

    #define LOGIC_BATCH 1024
    nsr_observation_t observations[LOGIC_BATCH];

    while (1) {
        uint64_t now_us = ttak_get_tick_count_ns() / 1000;
        uint32_t interval_ms = atomic_load(&config->interval_ms);
        g_state.interval_ms = interval_ms;

        // [1] EMIT PROBE INTENT (Throttled via Timeline Logic)
        if (!g_paused && (now_us - last_probe_us >= (uint64_t)interval_ms * 1000)) {
            // Send a full burst (round) of probes per interval tick for maximum throughput
            #define BURST_SIZE 30
            nsr_intent_t intents[BURST_SIZE];
            
            for (int i = 0; i < BURST_SIZE; i++) {
                intents[i].ttl = current_ttl;
                intents[i].seq = current_seq;
                intents[i].action = 0;
                intents[i].timestamp_us = now_us;
                intents[i].integrity = compute_integrity_fast(current_ttl, current_seq);
                
                g_state.hops[current_ttl].sent++;
                current_seq++;
                
                current_ttl++;
                if (__builtin_expect(current_ttl > 30, 0)) current_ttl = 1;
            }

            for (int i = 0; i < BURST_SIZE; i++) {
                while (!nsr_shm_ring_push_batch(l2g, &intents[i], 1, sizeof(nsr_intent_t)));
            }
            
            last_probe_us = now_us;
        } else if (g_paused) {
            // If paused, just sleep a bit to avoid busy wait
            struct timespec ts = {0, 10000000}; // 10ms
            nanosleep(&ts, NULL);
        } else {
            // Pacing sleep: wait for a fraction of the interval or 1ms
            struct timespec ts = {0, 1000000}; // 1ms
            nanosleep(&ts, NULL);
        }

        // [2] DRAIN OBSERVATIONS (SHM Pop Batch)
        while (1) {
            int n_obs = nsr_shm_ring_pop_batch(g2l, observations, LOGIC_BATCH, sizeof(nsr_observation_t));
            if (n_obs <= 0) break;
            for (int i = 0; i < n_obs; i++) {
                update_state(observations[i].ttl, &observations[i]);
            }
        }

        // [3] PERIODIC TUI UPDATE
        static uint64_t last_tui_us = 0;
        if (now_us - last_tui_us > 16666) {
            nsr_shm_ring_large_push(l2t, &g_state, sizeof(g_state));
            last_tui_us = now_us;
        }
    }
}
