#include <nsr/singular.h>
#include <ttak/net/core/icmp.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdio.h>

void nsr_singular_receiver_main(int evt_pipe_fd) {
    int fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (fd < 0) {
        perror("[RECEIVER] socket");
        return;
    }

    while (1) {
        uint8_t buf[2048];
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            struct iphdr *ip = (struct iphdr *)buf;
            struct icmphdr *icmp = (struct icmphdr *)(buf + (ip->ihl * 4));
            
            uint16_t seq = 0;
            bool valid = false;

            if (icmp->type == ICMP_ECHOREPLY) {
                seq = ntohs(icmp->un.echo.sequence);
                valid = true;
            } else if (icmp->type == ICMP_TIME_EXCEEDED) {
                struct iphdr *inner_ip = (struct iphdr *)((uint8_t*)icmp + 8);
                struct icmphdr *inner_icmp = (struct icmphdr *)((uint8_t*)inner_ip + (inner_ip->ihl * 4));
                seq = ntohs(inner_icmp->un.echo.sequence);
                valid = true;
            }

            if (valid) {
                nsr_msg_t event = {
                    .magic = NSR_MSG_MAGIC,
                    .type = 1, // RECV_EVT
                    .seq = seq,
                    .ts_us = ttak_get_tick_count_ns() / 1000
                };
                write(evt_pipe_fd, &event, sizeof(event));
            }
        }
    }
}
