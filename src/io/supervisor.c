#include <nsr/io/supervisor.h>
#include <ttak/net/core/icmp.h>
#include <string.h>
#include <stdlib.h>

void nsr_worker_tracer(nsr_cap_t cap, const char *target)
{
    (void)target;
    ttak_sys_restrict(TTAK_RESTRICT_IO_NET | TTAK_RESTRICT_MEM_SHM);

    ttak_net_session_t *net = ttak_net_session_create(TTAK_NET_PROTO_ICMP);

    while (true) {
        ttak_epoch_enter();

        for (uint8_t ttl = 1; ttl <= 15; ttl++) {
            nsr_hop_record_t local_hop;

            if (nsr_state_get_hop(cap, ttl, &local_hop)) {
                if (local_hop.state != 1) {
                    local_hop.generation++;
                    local_hop.state = 1;
                    local_hop.sent_at_us = ttak_timing_now_us();

                    nsr_state_update_hop(cap, ttl, &local_hop);

                    ttak_mem_block_t *pkt = ttak_mem_alloc(NULL, sizeof(ttak_icmp_v4_hdr_t));
                    (void)pkt;
                    (void)net;
                }
            }
        }

        ttak_epoch_exit();
        ttak_timing_sleep(ttak_duration_from_ms(100));
    }
}

void nsr_supervisor_main(const char *target)
{
    nsr_cap_t cap = ttak_shm_create_isolated("nsr_state", sizeof(nsr_shared_state_t));

    nsr_shared_state_t *state = (nsr_shared_state_t *)ttak_shm_map(cap);
    memset(state, 0, sizeof(*state));
    state->magic = 0x52534E21;
    state->version = 1;
    ttak_shm_unmap(state);

    ttak_supervisor_t *sv = ttak_supervisor_create();
    ttak_supervisor_spawn(sv, (void *)nsr_worker_tracer, cap, target);

    nsr_tui_supervisor_display(cap);
}

bool nsr_state_get_hop(nsr_cap_t cap, uint8_t ttl, nsr_hop_record_t *out)
{
    if (ttl >= NSR_MAX_HOPS)
        return false;

    nsr_shared_state_t *state = (nsr_shared_state_t *)ttak_shm_map(cap);
    if (!state)
        return false;

    memcpy(out, &state->hops[ttl], sizeof(nsr_hop_record_t));

    ttak_shm_unmap(state);
    return true;
}

bool nsr_state_update_hop(nsr_cap_t cap, uint8_t ttl, const nsr_hop_record_t *in)
{
    if (ttl >= NSR_MAX_HOPS)
        return false;

    nsr_shared_state_t *state = (nsr_shared_state_t *)ttak_shm_map(cap);
    if (!state)
        return false;

    state->hops[ttl] = *in;
    atomic_fetch_add(&state->global_generation, 1);

    ttak_shm_unmap(state);
    return true;
}
