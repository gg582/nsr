#include <nsr/io/singular.h>
#include <ttak/net/core/icmp.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdio.h>

void nsr_singular_sender_main(int cmd_pipe_fd) {
    int fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (fd < 0) {
        perror("[SENDER] socket");
        return;
    }

    // Fixed destination for this simplified singular mode
    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    inet_pton(AF_INET, "8.8.8.8", &dest.sin_addr);

    uint16_t id = (uint16_t)getpid();

    while (1) {
        nsr_msg_t cmd;
        if (read(cmd_pipe_fd, &cmd, sizeof(cmd)) == sizeof(cmd)) {
            if (cmd.magic == NSR_MSG_MAGIC && cmd.type == 0 /* SEND_REQ */) {
                int ttl = (int)cmd.ttl;
                setsockopt(fd, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl));

                ttak_net_icmpv4_hdr_t icmp;
                memset(&icmp, 0, sizeof(icmp));
                icmp.type = TTAK_ICMP_ECHO_REQUEST;
                icmp.un.echo.id = htons(id);
                icmp.un.echo.sequence = htons(cmd.seq);
                icmp.checksum = ttak_net_icmp_calculate_checksum(&icmp, sizeof(icmp));

                sendto(fd, &icmp, sizeof(icmp), 0, (struct sockaddr *)&dest, sizeof(dest));
            }
        }
    }
}
