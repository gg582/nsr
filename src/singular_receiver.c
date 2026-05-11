#include <nsr/singular.h>
#include <ttak/net/session.h>
#include <unistd.h>

/**
 * @brief RECEIVER Process: Read-Only Network.
 */
void nsr_singular_receiver_main(int evt_pipe_fd) {
    // HARDWARE ENFORCED: No WRITE to net, No READ from pipes.
    ttak_sys_restrict(TTAK_RESTRICT_READ_NET | TTAK_RESTRICT_WRITE_PIPE);

    ttak_net_session_t *net = ttak_net_session_create(TTAK_NET_PROTO_ICMP);

    while (true) {
        ttak_net_packet_t *pkt = ttak_net_recv(net);
        if (pkt) {
            nsr_msg_t event = {
                .magic = NSR_MSG_MAGIC,
                .type = 1, // RECV_EVT
                .ttl = 0,  // In real life, extract from ICMP
                .seq = 0,  // Extract from ICMP
                .ts_us = ttak_timing_now_us()
            };
            write(evt_pipe_fd, &event, sizeof(event));
            ttak_net_packet_free(pkt);
        }
    }
}
