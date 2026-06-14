#ifndef NSR_SUPERVISOR_H
#define NSR_SUPERVISOR_H

#include <ttak/process/supervisor.h>
#include <ttak/mem/shm.h>
#include <ttak/sync/ring_buffer.h>
#include <ttak/mem/epoch.h>

typedef uint64_t nsr_cap_t;

#define NSR_MAX_HOPS 64

typedef struct {
    uint32_t generation;
    uint8_t  ttl;
    uint16_t seq;
    uint32_t state;
    uint64_t sent_at_us;
    uint64_t rtt_us;
} nsr_hop_record_t;

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

bool nsr_state_get_hop(nsr_cap_t cap, uint8_t ttl, nsr_hop_record_t *out);
bool nsr_state_update_hop(nsr_cap_t cap, uint8_t ttl, const nsr_hop_record_t *in);

void nsr_supervisor_main(const char *target);
void nsr_worker_tracer(nsr_cap_t cap, const char *target);
void nsr_tui_supervisor_display(nsr_cap_t cap);

#endif
