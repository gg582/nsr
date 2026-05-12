#include <nsr/omni.h>
#include <ttak/timing/timing.h>
#include <ttak/security/siphash.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

static uint64_t compute_integrity_fast(uint8_t ttl, uint16_t seq) {
    return ((uint64_t)ttl << 16) | (uint64_t)seq;
}

int main() {
    printf("--- NSR MICRO-BENCHMARK (New Logic) ---\n");
    
    // Test 1: Fast Integrity Calculation Speed
    uint64_t start = ttak_get_tick_count_ns();
    uint64_t count = 10000000; // Increase count for more precision
    uint64_t sum = 0;
    for (uint64_t i = 0; i < count; i++) {
        sum += compute_integrity_fast((uint8_t)(i % 64), (uint16_t)i);
    }
    uint64_t end = ttak_get_tick_count_ns();
    printf("Fast Integrity speed: %.2f ns/op (Total: %lu ms for 10M ops)\n", 
           (double)(end - start) / count, (end - start) / 1000000);
    (void)sum;

    // Test 2: Memory Footprint (Informational)
    printf("nsr_omni_state_t size: %lu bytes\n", sizeof(nsr_omni_state_t));
    printf("nsr_intent_t size: %lu bytes\n", sizeof(nsr_intent_t));
    printf("nsr_observation_t size: %lu bytes\n", sizeof(nsr_observation_t));

    // Test 3: Binary Size
    // (This would be checked via shell command in a real harness)

    return 0;
}
