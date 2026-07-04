/**
 * @file logic.c
 * @brief High-efficiency tracer logic engine for NSR.
 *
 * Responsibilities:
 *   - TTL scheduling with adaptive burst pacing (AIMD).
 *   - Deterministic state progression including timeout detection.
 *   - Multi-node (ECMP) alias tracking per TTL.
 *   - Telemetry publishing to the TUI renderer.
 */

#include <nsr/telemetry.h>
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
    bool    is_v6;
} nsr_hop_addr_cache_t;

static nsr_telemetry_state_t g_state;
static nsr_hop_addr_cache_t  g_addr_cache[NSR_MAX_HOPS];
static volatile bool         g_paused = false;
static uint64_t              g_last_recv_us[NSR_MAX_HOPS];

/* ------------------------------------------------------------------ */
/* Adaptive pacing state (AIMD)                                       */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t current_burst;
    uint32_t max_burst;
    uint32_t min_burst;
    uint64_t last_adjust_us;
    uint32_t last_sent;
    uint32_t last_recv;
    float    ewma_loss;
} nsr_pacing_t;

static nsr_pacing_t g_pacing = {
    .current_burst = 28,
    .max_burst     = 64,
    .min_burst     = 1,
    .ewma_loss     = 0.0f,
};

static void handle_sigusr1(int sig)
{
    (void)sig;
    g_paused = !g_paused;
}

static inline uint64_t compute_integrity_fast(uint8_t ttl, uint16_t seq)
{
    return ((uint64_t)ttl << 16) | (uint64_t)seq;
}

static void addr_bin_to_str(const uint8_t *addr_bin, bool is_v6, char *out, size_t out_len)
{
    if (is_v6) {
        inet_ntop(AF_INET6, addr_bin, out, (socklen_t)out_len);
    } else {
        struct in_addr ia;
        memcpy(&ia, addr_bin, 4);
        inet_ntop(AF_INET, &ia, out, (socklen_t)out_len);
    }
}

static void add_or_update_alias(nsr_hop_info_t *h, const char *addr_str)
{
    if (h->addr[0] == '\0') {
        strncpy(h->addr, addr_str, sizeof(h->addr) - 1);
        h->primary_recv = 1;
        return;
    }
    if (strcmp(h->addr, addr_str) == 0) {
        h->primary_recv++;
        return;
    }

    for (uint8_t i = 0; i < h->alias_count; i++) {
        if (strcmp(h->aliases[i].addr, addr_str) == 0) {
            h->aliases[i].recv++;
            return;
        }
    }

    if (h->alias_count < NSR_MAX_HOP_ALIASES) {
        strncpy(h->aliases[h->alias_count].addr, addr_str, sizeof(h->aliases[0].addr) - 1);
        h->aliases[h->alias_count].recv = 1;
        h->alias_count++;
    } else {
        /* LRU eviction: replace least-seen alias. */
        uint32_t min_recv = h->aliases[0].recv;
        int      min_idx  = 0;
        for (int i = 1; i < NSR_MAX_HOP_ALIASES; i++) {
            if (h->aliases[i].recv < min_recv) {
                min_recv = h->aliases[i].recv;
                min_idx  = i;
            }
        }
        strncpy(h->aliases[min_idx].addr, addr_str, sizeof(h->aliases[0].addr) - 1);
        h->aliases[min_idx].recv = 1;
    }
}

static void update_state(uint8_t ttl, nsr_observation_t *obs)
{
    if (ttl >= NSR_MAX_HOPS || ttl == 0)
        return;
    nsr_hop_info_t *h = &g_state.hops[ttl];

    if (obs->type == NSR_OBS_REPLY ||
        obs->type == NSR_OBS_EXCEEDED ||
        obs->type == NSR_OBS_UNREACH) {
        h->recv++;
        h->rtt_us     = obs->rtt_us;
        h->last_status = obs->type;
        g_last_recv_us[ttl] = ttak_get_tick_count_ns() / 1000;

        char addr_str[48];
        addr_bin_to_str(obs->addr_bin, obs->is_v6, addr_str, sizeof(addr_str));
        add_or_update_alias(h, addr_str);
    } else if (obs->type == NSR_OBS_TIMEOUT) {
        h->last_status = NSR_OBS_TIMEOUT;
    }
}

/* ------------------------------------------------------------------ */
/* Timeout sweep: mark hops without recent activity as timed out.     */
/* ------------------------------------------------------------------ */
static void sweep_timeouts(uint64_t now_us)
{
    for (int i = 1; i < NSR_MAX_HOPS; i++) {
        nsr_hop_info_t *h = &g_state.hops[i];
        if (h->sent == 0)
            continue;
        if (h->last_status == NSR_OBS_REPLY || h->last_status == NSR_OBS_UNREACH)
            continue;

        uint64_t last_activity = g_last_recv_us[i];
        if (last_activity == 0)
            last_activity = g_state.start_time_us;

        if (now_us - last_activity > 1500000ULL)   /* 1.5 s timeout window */
            h->last_status = NSR_OBS_TIMEOUT;
    }
}

/* ------------------------------------------------------------------ */
/* AIMD pacing adjustment every 250 ms.                               */
/* ------------------------------------------------------------------ */
static void adjust_pacing(uint64_t now_us)
{
    if (now_us - g_pacing.last_adjust_us < 250000ULL)
        return;

    uint32_t total_sent = 0, total_recv = 0;
    for (int i = 1; i < NSR_MAX_HOPS; i++) {
        total_sent += g_state.hops[i].sent;
        total_recv += g_state.hops[i].recv;
    }

    uint32_t delta_sent = total_sent - g_pacing.last_sent;
    uint32_t delta_recv = total_recv - g_pacing.last_recv;

    g_pacing.last_sent = total_sent;
    g_pacing.last_recv = total_recv;
    g_pacing.last_adjust_us = now_us;

    if (delta_sent == 0)
        return;

    float instant_loss = 1.0f - (float)delta_recv / (float)delta_sent;
    g_pacing.ewma_loss = g_pacing.ewma_loss * 0.7f + instant_loss * 0.3f;

    if (g_pacing.ewma_loss > 0.35f) {
        /* Multiplicative decrease: back off by 25 %. */
        g_pacing.current_burst = (g_pacing.current_burst * 3U) / 4U;
        if (g_pacing.current_burst < g_pacing.min_burst)
            g_pacing.current_burst = g_pacing.min_burst;
    } else if (g_pacing.ewma_loss < 0.10f) {
        /* Additive increase: probe for more headroom. */
        if (g_pacing.current_burst < g_pacing.max_burst)
            g_pacing.current_burst++;
    }
}

void nsr_logic_run(nsr_shm_ring_t *g2l, nsr_shm_ring_t *l2g,
                   nsr_shm_ring_large_t *l2t, nsr_config_t *config)
{
    signal(SIGUSR1, handle_sigusr1);
    memset(&g_state, 0, sizeof(g_state));
    memset(g_addr_cache, 0, sizeof(g_addr_cache));
    memset(g_last_recv_us, 0, sizeof(g_last_recv_us));
    if (config && config->target_ip[0])
        strncpy(g_state.target_ip, config->target_ip, sizeof(g_state.target_ip) - 1);
    if (config && config->target_host[0])
        strncpy(g_state.target_host, config->target_host, sizeof(g_state.target_host) - 1);
    g_state.start_time_us = ttak_get_tick_count_ns() / 1000;

    uint8_t  current_ttl = 1;
    uint16_t current_seq = 0;
    uint64_t last_probe_us = 0;

#define LOGIC_BATCH 1024
#define MAX_BURST   64
    nsr_observation_t observations[LOGIC_BATCH];
    nsr_intent_t      intents[MAX_BURST];

    while (1) {
        uint64_t now_us = ttak_get_tick_count_ns() / 1000;
        uint32_t interval_ms = atomic_load(&config->interval_ms);
        g_state.interval_ms  = interval_ms;

        adjust_pacing(now_us);
        sweep_timeouts(now_us);

        if (!g_paused && (now_us - last_probe_us >= (uint64_t)interval_ms * 1000ULL)) {
            uint32_t burst = g_pacing.current_burst;
            if (burst > MAX_BURST)
                burst = MAX_BURST;

            for (uint32_t i = 0; i < burst; i++) {
                intents[i].ttl         = current_ttl;
                intents[i].seq         = current_seq;
                intents[i].action      = 0;
                intents[i].timestamp_us = now_us;
                intents[i].integrity   = compute_integrity_fast(current_ttl, current_seq);

                g_state.hops[current_ttl].sent++;
                current_seq++;
                current_ttl++;
                if (__builtin_expect(current_ttl > 30, 0))
                    current_ttl = 1;
            }

            for (uint32_t i = 0; i < burst; i++) {
                while (!nsr_shm_ring_push_batch(l2g, &intents[i], 1, sizeof(nsr_intent_t)))
                    ;
            }

            last_probe_us = now_us;
        } else if (g_paused) {
            struct timespec ts = {0, 10000000};
            nanosleep(&ts, nullptr);
        } else {
            struct timespec ts = {0, 1000000};
            nanosleep(&ts, nullptr);
        }

        while (1) {
            int n_obs = nsr_shm_ring_pop_batch(g2l, observations, LOGIC_BATCH, sizeof(nsr_observation_t));
            if (n_obs <= 0)
                break;
            for (int i = 0; i < n_obs; i++)
                update_state(observations[i].ttl, &observations[i]);
        }

        static uint64_t last_tui_us = 0;
        if (now_us - last_tui_us > 16666ULL) {
            nsr_shm_ring_large_push(l2t, &g_state, sizeof(g_state));
            last_tui_us = now_us;
        }
    }
}
