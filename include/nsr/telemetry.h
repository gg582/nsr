#ifndef NSR_TELEMETRY_H
#define NSR_TELEMETRY_H

#define _XOPEN_SOURCE 700
#include <stdint.h>
#include <stdbool.h>
#include <ttak/types/ttak_compiler.h>

typedef enum {
    NSR_OBS_NONE = 0,
    NSR_OBS_REPLY = 1,
    NSR_OBS_EXCEEDED = 2,
    NSR_OBS_UNREACH = 3,
    NSR_OBS_TIMEOUT = 4,
    NSR_OBS_ERROR = 5
} nsr_obs_type_t;

typedef struct {
    uint8_t  ttl;
    uint16_t seq;
    uint32_t action;
    uint64_t timestamp_us;
    uint64_t integrity;
} nsr_intent_t;

typedef struct {
    uint8_t  ttl;
    uint16_t seq;
    uint64_t rtt_us;
    nsr_obs_type_t type;
    uint8_t  addr_bin[16];
    bool     is_v6;
} nsr_observation_t;

#define NSR_MAX_HOPS 64
#define NSR_GK_MAX_INFLIGHT 65536
#define NSR_INTEGRITY_KEY0 0xDEADC0DECAFEBABEULL
#define NSR_INTEGRITY_KEY1 0xFEEDFACEBEEFCAFEULL

#include <stdatomic.h>

typedef struct {
    atomic_uint interval_ms;
    char target_ip[48];
    char target_host[128];
} nsr_config_t;

#define NSR_MAX_HOP_ALIASES 3

typedef struct {
    char     addr[48];
    uint32_t recv;
    uint32_t _pad;
} nsr_hop_alias_t;

typedef struct {
    char             addr[48];
    uint64_t         rtt_us;
    uint32_t         sent;
    uint32_t         recv;
    uint32_t         primary_recv;
    nsr_obs_type_t   last_status;
    uint8_t          alias_count;
    uint8_t          _pad[3];
    nsr_hop_alias_t  aliases[NSR_MAX_HOP_ALIASES];
} nsr_hop_info_t;

typedef struct {
    nsr_hop_info_t  hops[NSR_MAX_HOPS];
    char            target_ip[48];
    char            target_host[128];
    uint64_t        start_time_us;
    uint32_t        interval_ms;
} nsr_telemetry_state_t;

_Static_assert(sizeof(nsr_telemetry_state_t) <= 16384,
               "nsr_telemetry_state_t must fit in shm_ring_large slot");

#include <nsr/shm_ring.h>

void nsr_gatekeeper_run(nsr_shm_ring_t *l2g, nsr_shm_ring_t *g2l, const char *target);
void nsr_logic_run(nsr_shm_ring_t *g2l, nsr_shm_ring_t *l2g, nsr_shm_ring_large_t *l2t, nsr_config_t *config);

#endif
