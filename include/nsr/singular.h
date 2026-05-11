#ifndef NSR_SINGULAR_H
#define NSR_SINGULAR_H

#include <ttak/types/ttak_types.h>
#include <ttak/timing/timing.h>

/**
 * @brief Zero-Trust Event Message.
 * The only currency allowed between processes.
 */
typedef struct {
    uint64_t magic;      // Verification token
    uint8_t  ttl;
    uint16_t seq;
    uint8_t  type;       // 0: SEND_REQ, 1: RECV_EVT, 2: TIMEOUT_EVT
    uint64_t ts_us;
} nsr_msg_t;

#define NSR_MSG_MAGIC 0x53494E47554C4152ULL // "SINGULAR"

/**
 * @brief Pointer Masking: Obfuscates handles in memory.
 */
extern uint64_t g_nsr_mask;
#define MASK_HANDLE(h) ((uint64_t)(h) ^ g_nsr_mask)
#define UNMASK_HANDLE(h) ((void*)((h) ^ g_nsr_mask))

/* --- Process Entry Points --- */

/**
 * @brief The SENDER: Write-Only Network Sandbox.
 * Restricted to sending ICMP and reading command pipe.
 */
void nsr_singular_sender_main(int cmd_pipe_fd);

/**
 * @brief The RECEIVER: Read-Only Network Sandbox.
 * Restricted to receiving ICMP and writing event pipe.
 */
void nsr_singular_receiver_main(int evt_pipe_fd);

/**
 * @brief The BROKER: The Immutable State Master.
 * Orchestrates messages and maintains the event log.
 */
void nsr_singular_broker_main(const char *target);

#endif
