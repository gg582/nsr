#ifndef NSR_SHM_RING_H
#define NSR_SHM_RING_H

#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdalign.h>

#define SHM_RING_SIZE 65536

typedef union {
    uint8_t raw[128];
} nsr_shm_item_t;

typedef struct {
    alignas(64) atomic_uint_least32_t head;
    alignas(64) atomic_uint_least32_t tail;
    alignas(64) nsr_shm_item_t data[SHM_RING_SIZE];
} nsr_shm_ring_t;

static inline bool nsr_shm_ring_push_batch(nsr_shm_ring_t *r, const void *items, int count, size_t size) {
    uint32_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
    uint32_t t = atomic_load_explicit(&r->tail, memory_order_acquire);
    uint32_t next = (h + count) & (SHM_RING_SIZE - 1);
    
    // Check for overflow (simplified)
    uint32_t used = (h - t) & (SHM_RING_SIZE - 1);
    if (used + count >= SHM_RING_SIZE - 1) return false;

    for (int i = 0; i < count; i++) {
        memcpy(r->data[(h + i) & (SHM_RING_SIZE - 1)].raw, (uint8_t*)items + i * size, size);
    }
    
    atomic_store_explicit(&r->head, next, memory_order_release);
    return true;
}

static inline bool nsr_shm_ring_push(nsr_shm_ring_t *r, const void *item, size_t size) {
    return nsr_shm_ring_push_batch(r, item, 1, size);
}

static inline bool nsr_shm_ring_pop(nsr_shm_ring_t *r, void *out_item, size_t size) {
    uint32_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
    uint32_t h = atomic_load_explicit(&r->head, memory_order_acquire);
    if (__builtin_expect(t == h, 0)) return false;
    
    memcpy(out_item, r->data[t].raw, size);
    
    atomic_store_explicit(&r->tail, (t + 1) & (SHM_RING_SIZE - 1), memory_order_release);
    return true;
}

#endif
