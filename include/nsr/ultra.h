#ifndef NSR_ULTRA_H
#define NSR_ULTRA_H

#include <ttak/process/supervisor.h>
#include <ttak/mem/shm.h>
#include <ttak/sync/ring_buffer.h>
#include <ttak/mem/epoch.h>

/**
 * @brief Capability Handle - The ONLY way to reference state in NSR Ultra.
 * No raw pointers exist in the logical layer.
 */
typedef uint64_t nsr_cap_t;

#define NSR_MAX_HOPS 64

/**
 * @brief Ephemeral state for a single hop. 
 * Managed within Generational SHM.
 */
typedef struct {
    uint32_t generation;
    uint8_t  ttl;
    uint16_t seq;
    uint32_t state;
    uint64_t sent_at_us;
    uint64_t rtt_us;
} nsr_hop_record_t;

/**
 * @brief Global State Segment (Stored in Isolated SHM)
 */
typedef struct {
    uint32_t magic;
    uint32_t version;
    atomic_uint global_generation;
    nsr_hop_record_t hops[NSR_MAX_HOPS];
    struct {
        atomic_uint64_t sent;
        atomic_uint64_t recv;
        uint64_t srtt_us;
    } global_stats;
} nsr_shared_state_t;

/* --- The "Safe" API: No Pointers Allowed --- */

/**
 * @brief Atomically snapshots a hop record into local stack memory.
 * No "Long-lived" pointer to the state is ever returned.
 */
bool nsr_state_get_hop(nsr_cap_t cap, uint8_t ttl, nsr_hop_record_t *out);

/**
 * @brief Safe update of state via Capability.
 */
bool nsr_state_update_hop(nsr_cap_t cap, uint8_t ttl, const nsr_hop_record_t *in);

/* --- Lifecycle --- */
void nsr_ultra_supervisor_main(const char *target);
void nsr_ultra_worker_tracer(nsr_cap_t cap, const char *target);
void nsr_ultra_tui_display(nsr_cap_t cap);

#endif
