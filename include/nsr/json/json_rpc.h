#ifndef NSR_JSON_RPC_H
#define NSR_JSON_RPC_H

#include <nsr/json/json.h>
#include <stdbool.h>
#include <sys/types.h>

typedef struct {
    pid_t pid;
    int in;   /* write to child stdin */
    int out;  /* read from child stdout */
    int next_id;
    long long pending_id; /* id of the last request awaiting a response, -1 if none */
    bool dead;
} nsr_json_rpc_t;

bool nsr_json_rpc_spawn(nsr_json_rpc_t *rpc, const char *path);
void nsr_json_rpc_close(nsr_json_rpc_t *rpc);

/* Send a JSON-RPC request without waiting for a response.
 * If id_out is non-NULL it receives the request id used for matching. */
bool nsr_json_rpc_send_request(nsr_json_rpc_t *rpc,
                               const char *method,
                               const nsr_json_buf_t *params,
                               long long *id_out);

/* Try to receive the response for the last sent request.
 * Returns true if the matching response arrived within timeout_ms. */
bool nsr_json_rpc_try_recv_response(nsr_json_rpc_t *rpc,
                                    int timeout_ms,
                                    nsr_json_buf_t *response_out);

/* Receive the next available response without id matching. Useful when the
 * caller manages several concurrent request types and routes by id itself. */
bool nsr_json_rpc_try_recv_any(nsr_json_rpc_t *rpc,
                               int timeout_ms,
                               nsr_json_buf_t *response_out,
                               long long *id_out);

/* Synchronous call; kept for compatibility and simple callers. */
bool nsr_json_rpc_call(nsr_json_rpc_t *rpc,
                       const char *method,
                       const nsr_json_buf_t *params,
                       int timeout_ms,
                       nsr_json_buf_t *response_out);

bool nsr_json_rpc_notify(nsr_json_rpc_t *rpc,
                         const char *method,
                         const nsr_json_buf_t *params);

#endif
