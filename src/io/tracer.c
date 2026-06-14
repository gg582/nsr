#include <nsr/io/tracer.h>
#include <ttak/net/core/icmp.h>
#include <string.h>

static uint16_t nsr_checksum(void *vdata, size_t length) {
    uint8_t *data = (uint8_t *)vdata;
    uint32_t acc = 0xffff;
    for (size_t i = 0; i + 1 < length; i += 2) {
        uint16_t word; memcpy(&word, data + i, 2); acc += word;
        if (acc > 0xffff) acc -= 0xffff;
    }
    if (length & 1) {
        uint16_t word = 0; memcpy(&word, data + length - 1, 1); acc += word;
        if (acc > 0xffff) acc -= 0xffff;
    }
    return ~acc;
}

ttak_result_t nsr_init(nsr_context_t *ctx, const char *target_addr) {
    // 1. Create sandbox owner with strict isolation
    ctx->owner = ttak_owner_create(TTAK_OWNER_STRICT_ISOLATION | TTAK_OWNER_DENY_THREADING);
    if (!ctx->owner) return TTAK_ERR_NOMEM;

    ctx->net = ttak_net_session_create(TTAK_NET_PROTO_ICMP);
    
    // 2. Register logic functions to owner context
    ttak_owner_register_func(ctx->owner, "tick", nsr_logic_tick);
    ttak_owner_register_func(ctx->owner, "on_recv", nsr_logic_on_recv);
    ttak_owner_register_func(ctx->owner, "on_timeout", nsr_logic_on_timeout);

    // 3. Pointer-stable allocation using Abstract Memory
    for (int i = 0; i < NSR_MAX_HOPS; i++) {
        ctx->hops[i].abs_mem = ttak_abstract_alloc(sizeof(nsr_probe_data_t));
        ctx->hops[i].generation = 1;
        
        nsr_probe_data_t init_data = { .generation = 1, .seq = (uint16_t)i, .state = NSR_PROBE_IDLE };
        ttak_abstract_write(ctx->hops[i].abs_mem, 0, &init_data, sizeof(init_data));
    }

    ctx->running = true;
    ctx->start_time = ttak_timing_now();
    return TTAK_SUCCESS;
}

void nsr_logic_tick(void *user_ctx, void *args) {
    nsr_context_t *ctx = (nsr_context_t *)user_ctx;
    uint8_t ttl = *(uint8_t *)args;

    // Use EBR to protect the critical path of sending
    ttak_epoch_enter();
    
    nsr_probe_handle_t *h = &ctx->hops[ttl];
    nsr_probe_data_t data;
    ttak_abstract_read(h->abs_mem, 0, &data, sizeof(data));

    if (data.state != NSR_PROBE_SENT) {
        data.generation++;
        data.state = NSR_PROBE_SENT;
        data.sent_at = ttak_timing_now();
        ttak_abstract_write(h->abs_mem, 0, &data, sizeof(data));

        // Network transmission...
        ttak_mem_block_t *block = ttak_mem_alloc(NULL, sizeof(ttak_icmp_v4_hdr_t));
        ttak_icmp_v4_hdr_t *icmp = (ttak_icmp_v4_hdr_t *)block->ptr;
        icmp->type = TTAK_ICMP_ECHO_REQUEST;
        icmp->sequence = data.seq;
        icmp->checksum = 0;
        icmp->checksum = nsr_checksum(icmp, sizeof(*icmp));

        ttak_net_view_t view; ttak_net_view_init(&view);
        ttak_net_view_set_ttl(&view, ttl);
        
        // Final send (Abstracted)
        // ttak_net_send_to(ctx->net, block, ..., &view);
        
        ctx->stats.sent_count++;
    }

    ttak_epoch_exit();
}

void nsr_logic_on_timeout(void *user_ctx, void *args) {
    nsr_context_t *ctx = (nsr_context_t *)user_ctx;
    nsr_probe_handle_t *h = (nsr_probe_handle_t *)args;

    // Strict Pointer Sandboxing: Never access raw data directly
    ttak_epoch_enter();
    
    nsr_probe_data_t data;
    ttak_abstract_read(h->abs_mem, 0, &data, sizeof(data));

    // EBR + Generation Check
    if (data.generation == h->generation && data.state == NSR_PROBE_SENT) {
        data.state = NSR_PROBE_TIMEOUT;
        ttak_abstract_write(h->abs_mem, 0, &data, sizeof(data));
    }

    ttak_epoch_exit();
}

void nsr_cleanup(nsr_context_t *ctx) {
    for (int i = 0; i < NSR_MAX_HOPS; i++) {
        // Safe retirement using EBR
        ttak_epoch_retire(ctx->hops[i].abs_mem, (void (*)(void *))ttak_abstract_free);
    }
    ttak_owner_destroy(ctx->owner);
}
