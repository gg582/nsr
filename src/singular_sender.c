#include <nsr/singular.h>
#include <ttak/net/session.h>
#include <unistd.h>

/**
 * @brief SENDER Process: Write-Only Network.
 */
void nsr_singular_sender_main(int cmd_pipe_fd) {
    // HARDWARE ENFORCED: No READ from net, No WRITE to pipes except stderr.
    ttak_sys_restrict(TTAK_RESTRICT_WRITE_NET | TTAK_RESTRICT_READ_PIPE);
    
    ttak_net_session_t *net = ttak_net_session_create(TTAK_NET_PROTO_ICMP);

    while (true) {
        nsr_msg_t cmd;
        if (read(cmd_pipe_fd, &cmd, sizeof(cmd)) == sizeof(cmd)) {
            if (cmd.magic == NSR_MSG_MAGIC && cmd.type == 0 /* SEND_REQ */) {
                // Construct and send packet (Simplified)
                // ttak_net_send_icmp(net, cmd.ttl, cmd.seq);
            }
        }
    }
}
