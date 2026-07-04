#include <nsr/state/topology.h>
#include <ttak/timing/timing.h>
#include <string.h>
#include <math.h>

void nsr_topology_init(nsr_topology_state_t *topo)
{
    memset(topo, 0, sizeof(*topo));
    atomic_init(&topo->node_count, 0);
    atomic_init(&topo->edge_count, 0);
    topo->next_id = 1;
}

nsr_topology_node_t *nsr_topology_find_by_addr_and_target(nsr_topology_state_t *topo,
                                                           const char *addr,
                                                           const char *target_ip)
{
    for (int i = 0; i < NSR_TOPOLOGY_MAX_NODES; i++) {
        nsr_topology_node_t *n = &topo->nodes[i];
        if (n->active && strcmp(n->addr, addr) == 0 && strcmp(n->target_ip, target_ip) == 0)
            return n;
    }
    return nullptr;
}

uint32_t nsr_topology_upsert_node(nsr_topology_state_t *topo,
                                  const char *addr,
                                  uint8_t ttl,
                                  const char *target_ip,
                                  bool is_destination)
{
    if (!addr || !addr[0] || !target_ip || !target_ip[0])
        return 0;

    nsr_topology_node_t *node = nsr_topology_find_by_addr_and_target(topo, addr, target_ip);
    if (node) {
        node->ttl = ttl;
        node->is_destination = is_destination;
        return node->id;
    }

    uint32_t count = atomic_load(&topo->node_count);
    if (count >= NSR_TOPOLOGY_MAX_NODES)
        return 0;

    uint32_t id = topo->next_id++;
    node = &topo->nodes[id % NSR_TOPOLOGY_MAX_NODES];
    memset(node, 0, sizeof(*node));
    node->id = id;
    node->ttl = ttl;
    node->active = true;
    node->is_destination = is_destination;
    strncpy(node->addr, addr, sizeof(node->addr) - 1);
    strncpy(node->target_ip, target_ip, sizeof(node->target_ip) - 1);
    atomic_fetch_add(&topo->node_count, 1);
    return id;
}

static void add_edge(nsr_topology_state_t *topo, uint32_t from_id, uint32_t to_id)
{
    if (from_id == 0 || to_id == 0 || from_id == to_id)
        return;

    uint32_t ecount = atomic_load(&topo->edge_count);
    if (ecount >= NSR_TOPOLOGY_MAX_NODES * 2)
        return;

    for (int i = 0; i < NSR_TOPOLOGY_MAX_NODES * 2; i++) {
        nsr_topology_edge_t *e = &topo->edges[i];
        if (e->active && e->from == from_id && e->to == to_id)
            return;
    }

    for (int i = 0; i < NSR_TOPOLOGY_MAX_NODES * 2; i++) {
        nsr_topology_edge_t *e = &topo->edges[i];
        if (!e->active) {
            e->from = from_id;
            e->to = to_id;
            e->active = true;
            atomic_fetch_add(&topo->edge_count, 1);
            return;
        }
    }
}

static nsr_topology_node_t *node_by_id(nsr_topology_state_t *topo, uint32_t id)
{
    for (int i = 0; i < NSR_TOPOLOGY_MAX_NODES; i++) {
        if (topo->nodes[i].active && topo->nodes[i].id == id)
            return &topo->nodes[i];
    }
    return nullptr;
}

static void update_node_health(nsr_topology_node_t *node, const nsr_hop_info_t *h,
                               uint32_t specific_recv)
{
    uint32_t prev_recv = node->health.recv;
    node->health.rtt_us      = (float)h->rtt_us;
    node->health.last_rtt_us = h->rtt_us;
    node->health.sent        = h->sent;
    node->health.recv        = specific_recv;
    node->health.timeouts    = h->sent > specific_recv ? h->sent - specific_recv : 0;
    if (h->sent > 0)
        node->health.loss_rate = 1.0f - (float)specific_recv / h->sent;

    if (prev_recv != specific_recv && specific_recv > 0) {
        float delta = fabsf((float)node->health.last_rtt_us - node->health.rtt_us);
        node->health.jitter_us = node->health.jitter_us * 0.8f + delta * 0.2f;
    }

    node->health.last_update_us = ttak_get_tick_count_ns() / 1000;
}

void nsr_topology_update_from_telemetry(nsr_topology_state_t *topo,
                                        const nsr_telemetry_state_t *tel)
{
    const char *target_ip = tel->target_ip;
    if (!target_ip[0])
        return;

    /* Destination node for this target (ttl will be fixed after scanning). */
    uint32_t dest_id = nsr_topology_upsert_node(topo, target_ip, 0, target_ip, true);

    uint32_t hop_ids[NSR_MAX_HOPS][1 + NSR_MAX_HOP_ALIASES];
    memset(hop_ids, 0, sizeof(hop_ids));

    /* Upsert all nodes per TTL. */
    for (int i = 1; i < NSR_MAX_HOPS; i++) {
        const nsr_hop_info_t *h = &tel->hops[i];
        if (!h->addr[0])
            continue;

        hop_ids[i][0] = nsr_topology_upsert_node(topo, h->addr, (uint8_t)i, target_ip, false);
        if (hop_ids[i][0] != 0) {
            nsr_topology_node_t *node = node_by_id(topo, hop_ids[i][0]);
            if (node)
                update_node_health(node, h, h->primary_recv);
        }

        for (uint8_t a = 0; a < h->alias_count; a++) {
            hop_ids[i][1 + a] = nsr_topology_upsert_node(topo, h->aliases[a].addr, (uint8_t)i, target_ip, false);
            if (hop_ids[i][1 + a] != 0) {
                nsr_topology_node_t *anode = node_by_id(topo, hop_ids[i][1 + a]);
                if (anode)
                    update_node_health(anode, h, h->aliases[a].recv);
            }
        }
    }

    /* Build edges between consecutive TTLs. */
    for (int i = 1; i < NSR_MAX_HOPS - 1; i++) {
        for (int src_idx = 0; src_idx < 1 + NSR_MAX_HOP_ALIASES; src_idx++) {
            uint32_t from_id = hop_ids[i][src_idx];
            if (from_id == 0)
                continue;

            bool any_next = false;
            for (int dst_idx = 0; dst_idx < 1 + NSR_MAX_HOP_ALIASES; dst_idx++) {
                uint32_t to_id = hop_ids[i + 1][dst_idx];
                if (to_id != 0) {
                    add_edge(topo, from_id, to_id);
                    any_next = true;
                }
            }

            /* If no next hop, link to destination (last hop). */
            if (!any_next && dest_id != 0) {
                add_edge(topo, from_id, dest_id);
            }
        }
    }

    /* Link last TTL hop(s) to destination if not already linked. */
    int last_ttl = 0;
    for (int i = NSR_MAX_HOPS - 1; i >= 1; i--) {
        if (hop_ids[i][0] != 0) {
            last_ttl = i;
            break;
        }
    }
    if (last_ttl > 0 && dest_id != 0) {
        for (int src_idx = 0; src_idx < 1 + NSR_MAX_HOP_ALIASES; src_idx++) {
            uint32_t from_id = hop_ids[last_ttl][src_idx];
            if (from_id != 0 && from_id != dest_id)
                add_edge(topo, from_id, dest_id);
        }
        nsr_topology_node_t *dest_node = node_by_id(topo, dest_id);
        if (dest_node)
            dest_node->ttl = (uint8_t)(last_ttl + 1);
    }
}

int nsr_topology_active_count(const nsr_topology_state_t *topo)
{
    return (int)atomic_load(&topo->node_count);
}

float nsr_health_score(const nsr_node_health_t *h)
{
    float loss_factor = h->loss_rate;
    float rtt_factor  = h->rtt_us / 200000.0f;
    if (rtt_factor > 1.0f)
        rtt_factor = 1.0f;
    float jitter_factor = h->jitter_us / 100000.0f;
    if (jitter_factor > 1.0f)
        jitter_factor = 1.0f;
    float inst_factor = h->instability;

    return loss_factor * 0.50f + rtt_factor * 0.20f + jitter_factor * 0.15f + inst_factor * 0.15f;
}

/* ============================================================
 * Pseudo-OOP topology manager implementation.
 * ============================================================ */

static void topo_mgr_init(nsr_topology_manager_t *self)
{
    nsr_topology_init(&self->state);
}

static void topo_mgr_update_from_telemetry(nsr_topology_manager_t *self,
                                           const nsr_telemetry_state_t *tel)
{
    nsr_topology_update_from_telemetry(&self->state, tel);
}

static int topo_mgr_active_count(const nsr_topology_manager_t *self)
{
    return nsr_topology_active_count(&self->state);
}

const struct nsr_topology_manager_vtable nsr_topology_manager_vtable = {
    .init = topo_mgr_init,
    .update_from_telemetry = topo_mgr_update_from_telemetry,
    .active_count = topo_mgr_active_count,
};
