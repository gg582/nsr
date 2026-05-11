/**
 * @file omni.h
 * @brief Full compatibility interface for NSR Omni.
 */

#ifndef NSR_OMNI_H
#define NSR_OMNI_H

#define _XOPEN_SOURCE 700
#include <stdint.h>
#include <stdbool.h>
#include <ttak/types/ttak_compiler.h>

/**
 * @brief Observation types matching ICMP responses.
 */
typedef enum {
    NSR_OBS_NONE = 0,
    NSR_OBS_REPLY = 1,      /**< ICMP Echo Reply (Target reached) */
    NSR_OBS_EXCEEDED = 2,   /**< ICMP Time Exceeded (Intermediate hop) */
    NSR_OBS_TIMEOUT = 3,    /**< Packet lost */
    NSR_OBS_ERROR = 4       /**< Network error */
} nsr_obs_type_t;

typedef struct {
    uint8_t  ttl;
    uint16_t seq;
    uint32_t action; // 0: PROBE
    uint64_t integrity;
} nsr_intent_t;

typedef struct {
    uint8_t  ttl;
    uint16_t seq;
    uint64_t rtt_us;
    nsr_obs_type_t type;
    char     addr[48]; /**< Source address of the response */
} nsr_observation_t;

/**
 * @brief State shared with TUI for rendering.
 */
typedef struct {
    char addr[48];
    uint64_t rtt_us;
    uint32_t sent;
    uint32_t recv;
    nsr_obs_type_t last_status;
} nsr_hop_info_t;

#define NSR_MAX_HOPS 64

typedef struct {
    nsr_hop_info_t hops[NSR_MAX_HOPS];
    char target_ip[48];
    uint64_t start_time_us;
} nsr_omni_state_t;

void nsr_omni_gatekeeper_main(int logic_to_gate_fd, int gate_to_logic_fd, const char *target);
void nsr_omni_logic_main(int gate_to_logic_fd, int logic_to_gate_fd, int logic_to_tui_fd);

#endif
