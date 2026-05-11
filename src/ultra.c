#include <nsr/ultra.h>
#include <ttak/net/core/icmp.h>
#include <string.h>
#include <stdlib.h>

/**
 * @brief Logic Implementation: The Worker Process (Fault-Isolated)
 * This process can CRASH at any time, and the Supervisor will resurrect it.
 */
void nsr_ultra_worker_tracer(nsr_cap_t cap, const char *target) {
    // 1. Enter Extreme Restricted Mode (Sandboxing)
    ttak_sys_restrict(TTAK_RESTRICT_IO_NET | TTAK_RESTRICT_MEM_SHM);
    
    ttak_net_session_t *net = ttak_net_session_create(TTAK_NET_PROTO_ICMP);
    
    while (true) {
        // Ephemeral Loop: All variables are local to this epoch
        ttak_epoch_enter();

        for (uint8_t ttl = 1; ttl <= 15; ttl++) {
            nsr_hop_record_t local_hop;
            
            // Capability-based access: No long-lived pointers
            if (nsr_state_get_hop(cap, ttl, &local_hop)) {
                if (local_hop.state != 1 /* PROBING */) {
                    
                    // Update ephemeral local copy
                    local_hop.generation++;
                    local_hop.state = 1; 
                    local_hop.sent_at_us = ttak_timing_now_us();
                    
                    // Commit to SHM via Capability
                    nsr_state_update_hop(cap, ttl, &local_hop);

                    // Perform network I/O via LibTTAK abstractions
                    ttak_mem_block_t *pkt = ttak_mem_alloc(NULL, sizeof(ttak_icmp_v4_hdr_t));
                    // ... (ICMP packet prep) ...
                    // ttak_net_send_to(net, pkt, ...);
                }
            }
        }

        ttak_epoch_exit();
        ttak_timing_sleep(ttak_duration_from_ms(100));
        
        // Potential crash point for testing Supervisor recovery:
        // if (rand() % 100 == 0) abort(); 
    }
}

/**
 * @brief The Supervisor: The "Immortal" Process
 * Manages the lifecycle and Shared Memory.
 */
void nsr_ultra_supervisor_main(const char *target) {
    // 1. Initialize Generational SHM (Capability Source)
    nsr_cap_t cap = ttak_shm_create_isolated("nsr_ultra_state", sizeof(nsr_shared_state_t));
    
    // 2. Initialize Shared State (Zero-Pointers)
    nsr_shared_state_t *state = (nsr_shared_state_t *)ttak_shm_map(cap);
    memset(state, 0, sizeof(*state));
    state->magic = 0x52534E21; // "NSR!"
    state->version = 1;
    ttak_shm_unmap(state);

    // 3. Spawn Fault-Isolated Worker
    // If nsr_ultra_worker_tracer crashes, the supervisor restarts it immediately.
    ttak_supervisor_t *sv = ttak_supervisor_create();
    ttak_supervisor_spawn(sv, (void*)nsr_ultra_worker_tracer, cap, target);

    // 4. Run TUI in the Supervisor Context (or a separate child)
    nsr_ultra_tui_display(cap);
}

/**
 * @brief Capability Implementation (Internal)
 */
bool nsr_state_get_hop(nsr_cap_t cap, uint8_t ttl, nsr_hop_record_t *out) {
    if (ttl >= NSR_MAX_HOPS) return false;
    
    // Snapshot-based read: No pointers are leaked to the caller
    nsr_shared_state_t *state = (nsr_shared_state_t *)ttak_shm_map(cap);
    if (!state) return false;
    
    memcpy(out, &state->hops[ttl], sizeof(nsr_hop_record_t));
    
    ttak_shm_unmap(state);
    return true;
}

bool nsr_state_update_hop(nsr_cap_t cap, uint8_t ttl, const nsr_hop_record_t *in) {
    if (ttl >= NSR_MAX_HOPS) return false;
    
    nsr_shared_state_t *state = (nsr_shared_state_t *)ttak_shm_map(cap);
    if (!state) return false;

    // Generational Write Protection
    state->hops[ttl] = *in;
    atomic_fetch_add(&state->global_generation, 1);
    
    ttak_shm_unmap(state);
    return true;
}
