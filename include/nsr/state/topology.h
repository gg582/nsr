#ifndef NSR_TOPOLOGY_H
#define NSR_TOPOLOGY_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <nsr/telemetry.h>

#define NSR_TOPOLOGY_MAX_NODES 1024
#define NSR_NODE_ADDR_LEN 48
#define NSR_MAX_TARGETS 8

typedef struct {
    float loss_rate;
    float rtt_us;
    float jitter_us;
    float instability;
    uint32_t route_changes;
    uint32_t sent;
    uint32_t recv;
    uint32_t timeouts;
    uint64_t last_rtt_us;
    uint64_t last_update_us;
} nsr_node_health_t;

typedef struct {
    uint32_t id;
    char addr[NSR_NODE_ADDR_LEN];
    char target_ip[NSR_NODE_ADDR_LEN];
    uint8_t ttl;
    bool is_destination;
    atomic_bool control_plane;
    bool active;
    nsr_node_health_t health;
} nsr_topology_node_t;

typedef struct {
    uint32_t from;
    uint32_t to;
    bool active;
} nsr_topology_edge_t;

typedef struct {
    nsr_topology_node_t nodes[NSR_TOPOLOGY_MAX_NODES];
    nsr_topology_edge_t edges[NSR_TOPOLOGY_MAX_NODES * 2];
    atomic_uint node_count;
    atomic_uint edge_count;
    uint32_t next_id;
} nsr_topology_state_t;

void nsr_topology_init(nsr_topology_state_t *topo);
uint32_t nsr_topology_upsert_node(nsr_topology_state_t *topo, const char *addr, uint8_t ttl, const char *target_ip, bool is_destination);
void nsr_topology_update_from_telemetry(nsr_topology_state_t *topo, const nsr_telemetry_state_t *tel);
nsr_topology_node_t *nsr_topology_find_by_addr_and_target(nsr_topology_state_t *topo, const char *addr, const char *target_ip);
int nsr_topology_active_count(const nsr_topology_state_t *topo);
float nsr_health_score(const nsr_node_health_t *h);

/* ============================================================
 * Pseudo-OOP topology manager (vtable + opaque this).
 * ============================================================ */

typedef struct nsr_topology_manager nsr_topology_manager_t;

struct nsr_topology_manager_vtable {
    void (*init)(nsr_topology_manager_t *self);
    void (*update_from_telemetry)(nsr_topology_manager_t *self,
                                  const nsr_telemetry_state_t *tel);
    int (*active_count)(const nsr_topology_manager_t *self);
};

struct nsr_topology_manager {
    const struct nsr_topology_manager_vtable *vt;
    nsr_topology_state_t state;
};

extern const struct nsr_topology_manager_vtable nsr_topology_manager_vtable;

#endif
