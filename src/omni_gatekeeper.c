/**
 * @file omni_gatekeeper.c
 * @brief High-Performance Dual-Stack ICMP Gatekeeper for NSR.
 */

#include <nsr/omni.h>
#include <ttak/net/session.h>
#include <ttak/timing/timing.h>
#include <ttak/net/core/icmp.h>
#include <ttak/security/siphash.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <netinet/icmp6.h>
#include <netinet/ip6.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <fcntl.h>

typedef struct {
    uint16_t seq;
    uint8_t  ttl;
    uint64_t sent_us;
    bool     active;
} nsr_inflight_t;

static nsr_inflight_t g_inflight[NSR_GK_MAX_INFLIGHT];
static uint16_t g_gk_id = 0;

static void track_probe(uint8_t ttl, uint16_t seq, uint64_t now_us) {
    int idx = seq % NSR_GK_MAX_INFLIGHT;
    g_inflight[idx].ttl = ttl;
    g_inflight[idx].seq = seq;
    g_inflight[idx].sent_us = now_us;
    g_inflight[idx].active = true;
}

static bool find_probe(uint16_t seq, uint8_t *ttl, uint64_t *sent_us) {
    int idx = seq % NSR_GK_MAX_INFLIGHT;
    if (g_inflight[idx].active && g_inflight[idx].seq == seq) {
        *ttl = g_inflight[idx].ttl;
        *sent_us = g_inflight[idx].sent_us;
        g_inflight[idx].active = false;
        return true;
    }
    return false;
}

static uint64_t compute_integrity(uint8_t ttl, uint16_t seq) {
    uint64_t val = ((uint64_t)ttl << 16) | (uint64_t)seq;
    return ttak_siphash24_u64(val, NSR_INTEGRITY_KEY0, NSR_INTEGRITY_KEY1);
}

void nsr_omni_gatekeeper_main(int logic_to_gate_fd, int gate_to_logic_fd, const char *target_ip) {
    bool is_v6 = (strchr(target_ip, ':') != NULL);
    int domain = is_v6 ? AF_INET6 : AF_INET;
    int proto = is_v6 ? IPPROTO_ICMPV6 : IPPROTO_ICMP;

    g_gk_id = (uint16_t)getpid();

    int fd = socket(domain, SOCK_RAW, proto);
    if (fd < 0) {
        perror("[GK] Failed to open raw socket");
        exit(EXIT_FAILURE);
    }

    // Set non-blocking
    fcntl(fd, F_SETFL, O_NONBLOCK);
    fcntl(logic_to_gate_fd, F_SETFL, O_NONBLOCK);

    struct sockaddr_storage dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    if (is_v6) {
        struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&dest_addr;
        addr6->sin6_family = AF_INET6;
        inet_pton(AF_INET6, target_ip, &addr6->sin6_addr);
    } else {
        struct sockaddr_in *addr4 = (struct sockaddr_in *)&dest_addr;
        addr4->sin_family = AF_INET;
        inet_pton(AF_INET, target_ip, &addr4->sin_addr);
    }

    struct pollfd fds[2];
    fds[0].fd = logic_to_gate_fd;
    fds[0].events = POLLIN;
    fds[1].fd = fd;
    fds[1].events = POLLIN;

    printf("[OMNI] Gatekeeper guarding target: %s (%s)\n", target_ip, is_v6 ? "IPv6" : "IPv4");

    while (1) {
        int ret = poll(fds, 2, 100);
        if (ret < 0) break;

        // [1] Handle New Intent from Logic
        if (fds[0].revents & POLLIN) {
            nsr_intent_t intent;
            if (read(logic_to_gate_fd, &intent, sizeof(intent)) == sizeof(intent)) {
                // Semantic Validation
                if (intent.ttl == 0 || intent.ttl >= NSR_MAX_HOPS) continue;
                if (intent.integrity != compute_integrity(intent.ttl, intent.seq)) {
                    fprintf(stderr, "[GK] Integrity failure: ttl=%d seq=%d\n", intent.ttl, intent.seq);
                    continue;
                }

                uint64_t now = ttak_get_tick_count_ns() / 1000;
                track_probe(intent.ttl, intent.seq, now);

                if (is_v6) {
                    int hops = (int)intent.ttl;
                    setsockopt(fd, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &hops, sizeof(hops));
                    struct {
                        struct icmp6_hdr hdr;
                        uint64_t ts;
                    } pkt;
                    memset(&pkt, 0, sizeof(pkt));
                    pkt.hdr.icmp6_type = ICMP6_ECHO_REQUEST;
                    pkt.hdr.icmp6_id = htons(g_gk_id);
                    pkt.hdr.icmp6_seq = htons(intent.seq);
                    pkt.ts = now;
                    sendto(fd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&dest_addr, sizeof(struct sockaddr_in6));
                } else {
                    int ttl = (int)intent.ttl;
                    setsockopt(fd, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl));
                    struct {
                        struct icmphdr hdr;
                        uint64_t ts;
                    } pkt;
                    memset(&pkt, 0, sizeof(pkt));
                    pkt.hdr.type = ICMP_ECHO;
                    pkt.hdr.un.echo.id = htons(g_gk_id);
                    pkt.hdr.un.echo.sequence = htons(intent.seq);
                    pkt.ts = now;
                    pkt.hdr.checksum = ttak_net_icmp_calculate_checksum(&pkt, sizeof(pkt));
                    sendto(fd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&dest_addr, sizeof(struct sockaddr_in));
                }
            }
        }

        // [2] Handle ICMP Response
        if (fds[1].revents & POLLIN) {
            uint8_t buf[2048];
            struct sockaddr_storage from_addr;
            socklen_t from_len = sizeof(from_addr);
            ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from_addr, &from_len);
            if (n > 0) {
                uint16_t res_seq = 0, res_id = 0;
                nsr_obs_type_t type = NSR_OBS_NONE;
                
                if (is_v6) {
                    struct icmp6_hdr *icmp6 = (struct icmp6_hdr *)buf;
                    if (icmp6->icmp6_type == ICMP6_ECHO_REPLY) {
                        res_id = ntohs(icmp6->icmp6_id);
                        res_seq = ntohs(icmp6->icmp6_seq);
                        type = NSR_OBS_REPLY;
                    } else if (icmp6->icmp6_type == ICMP6_TIME_EXCEEDED) {
                        // Extract from inner packet
                        struct ip6_hdr *inner_ip6 = (struct ip6_hdr *)(buf + sizeof(struct icmp6_hdr));
                        if (inner_ip6->ip6_nxt == IPPROTO_ICMPV6) {
                            struct icmp6_hdr *inner_icmp6 = (struct icmp6_hdr *)((uint8_t*)inner_ip6 + sizeof(struct ip6_hdr));
                            res_id = ntohs(inner_icmp6->icmp6_id);
                            res_seq = ntohs(inner_icmp6->icmp6_seq);
                            type = NSR_OBS_EXCEEDED;
                        }
                    }
                } else {
                    struct iphdr *ip = (struct iphdr *)buf;
                    struct icmphdr *icmp = (struct icmphdr *)(buf + (ip->ihl * 4));
                    if (icmp->type == ICMP_ECHOREPLY) {
                        res_id = ntohs(icmp->un.echo.id);
                        res_seq = ntohs(icmp->un.echo.sequence);
                        type = NSR_OBS_REPLY;
                    } else if (icmp->type == ICMP_TIME_EXCEEDED) {
                        struct iphdr *inner_ip = (struct iphdr *)((uint8_t*)icmp + 8);
                        struct icmphdr *inner_icmp = (struct icmphdr *)((uint8_t*)inner_ip + (inner_ip->ihl * 4));
                        res_id = ntohs(inner_icmp->un.echo.id);
                        res_seq = ntohs(inner_icmp->un.echo.sequence);
                        type = NSR_OBS_EXCEEDED;
                    }
                }

                if (type != NSR_OBS_NONE && res_id == g_gk_id) {
                    uint8_t ttl;
                    uint64_t sent_us;
                    if (find_probe(res_seq, &ttl, &sent_us)) {
                        nsr_observation_t obs;
                        memset(&obs, 0, sizeof(obs));
                        obs.ttl = ttl;
                        obs.seq = res_seq;
                        obs.type = type;
                        obs.rtt_us = (ttak_get_tick_count_ns() / 1000) - sent_us;
                        if (is_v6) {
                            inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&from_addr)->sin6_addr, obs.addr, sizeof(obs.addr));
                        } else {
                            inet_ntop(AF_INET, &((struct sockaddr_in *)&from_addr)->sin_addr, obs.addr, sizeof(obs.addr));
                        }
                        write(gate_to_logic_fd, &obs, sizeof(obs));
                    }
                }
            }
        }
    }
}
