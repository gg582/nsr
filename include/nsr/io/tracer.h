#ifndef NSR_TRACER_H
#define NSR_TRACER_H

#include <ttak/ttak_accelerator.h>
#include <ttak/mem/abstract.h>
#include <ttak/mem/owner.h>
#include <ttak/mem/epoch.h>
#include <ttak/net/session.h>
#include <ttak/timing/timing.h>

#define NSR_MAX_HOPS 64

/**
 * @brief Pointer-stable handle for a probe slot.
 */
typedef struct {
    ttak_abstract_mem_t *abs_mem;
    uint32_t generation;
} nsr_probe_handle_t;

typedef enum {
    NSR_PROBE_IDLE,
    NSR_PROBE_SENT,
    NSR_PROBE_RECEIVED,
    NSR_PROBE_TIMEOUT
} nsr_probe_state_t;

/**
 * @brief Internal data for a probe, stored in abstract memory.
 */
typedef struct {
    uint32_t generation;
    uint16_t seq;
    uint8_t ttl;
    ttak_timestamp_t sent_at;
    nsr_probe_state_t state;
    ttak_duration_t rtt;
    char last_ip[64];
} nsr_probe_data_t;

/**
 * @brief Global NSR Context, managed by a LibTTAK Owner.
 */
typedef struct {
    ttak_owner_t *owner;
    ttak_net_session_t *net;
    nsr_probe_handle_t hops[NSR_MAX_HOPS];
    
    struct {
        ttak_duration_t srtt;
        ttak_duration_t rttvar;
        uint64_t sent_count;
        uint64_t recv_count;
    } stats;
    
    bool running;
    ttak_timestamp_t start_time;
} nsr_context_t;

/* Extreme Safety Lifecycle */
ttak_result_t nsr_init(nsr_context_t *ctx, const char *target_addr);
void nsr_cleanup(nsr_context_t *ctx);

/* Owner-Executed Logic */
void nsr_logic_tick(void *ctx, void *args);
void nsr_logic_on_recv(void *ctx, void *args);
void nsr_logic_on_timeout(void *ctx, void *args);

#endif
