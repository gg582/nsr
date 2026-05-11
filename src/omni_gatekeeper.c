/**
 * @file omni_gatekeeper.c
 * @brief High-Performance Dual-Stack ICMP Gatekeeper for NSR.
 */

#define _GNU_SOURCE
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
#include <linux/errqueue.h>
#include <signal.h>

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

void nsr_omni_gatekeeper_omega(nsr_shm_ring_t *l2g, nsr_shm_ring_t *g2l, const char *target_ip) {
    bool is_v6 = (strchr(target_ip, ':') != NULL);
    int domain = is_v6 ? AF_INET6 : AF_INET;
    int proto = is_v6 ? IPPROTO_ICMPV6 : IPPROTO_ICMP;

    g_gk_id = (uint16_t)getpid();
    int fd = socket(domain, SOCK_RAW, proto);
    if (fd < 0) { perror("socket"); exit(1); }
    fcntl(fd, F_SETFL, O_NONBLOCK);

    int sndbuf = 16 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    struct sockaddr_storage dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    if (is_v6) {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)&dest_addr;
        a->sin6_family = AF_INET6;
        inet_pton(AF_INET6, target_ip, &a->sin6_addr);
    } else {
        struct sockaddr_in *a = (struct sockaddr_in *)&dest_addr;
        a->sin_family = AF_INET;
        inet_pton(AF_INET, target_ip, &a->sin_addr);
    }

    #define BATCH_SIZE 128
    struct mmsghdr msgs[BATCH_SIZE];
    struct iovec iovs[BATCH_SIZE];
    uint8_t pkts[BATCH_SIZE][128];
    uint8_t controls[BATCH_SIZE][CMSG_SPACE(sizeof(int))];

    memset(msgs, 0, sizeof(msgs));
    for (int i = 0; i < BATCH_SIZE; i++) {
        iovs[i].iov_base = pkts[i];
        msgs[i].msg_hdr.msg_iov = &iovs[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
        msgs[i].msg_hdr.msg_name = &dest_addr;
        msgs[i].msg_hdr.msg_namelen = is_v6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
        msgs[i].msg_hdr.msg_control = controls[i];
        msgs[i].msg_hdr.msg_controllen = sizeof(controls[i]);
    }

    while (1) {
        // [1] PULL INTENTS & BATCH SEND
        int count = 0;
        nsr_intent_t intent;
        while (count < BATCH_SIZE && nsr_shm_ring_pop(l2g, &intent, sizeof(intent))) {
            if (__builtin_expect(intent.integrity != compute_integrity(intent.ttl, intent.seq), 0)) continue;
            
            uint64_t now = ttak_get_tick_count_ns() / 1000;
            track_probe(intent.ttl, intent.seq, now);

            struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msgs[count].msg_hdr);
            if (is_v6) {
                cmsg->cmsg_level = IPPROTO_IPV6;
                cmsg->cmsg_type = IPV6_HOPLIMIT;
                cmsg->cmsg_len = CMSG_LEN(sizeof(int));
                *(int *)CMSG_DATA(cmsg) = intent.ttl;
                struct icmp6_hdr *h = (struct icmp6_hdr *)pkts[count];
                h->icmp6_type = ICMP6_ECHO_REQUEST; h->icmp6_code = 0; h->icmp6_cksum = 0;
                h->icmp6_id = htons(g_gk_id); h->icmp6_seq = htons(intent.seq);
                iovs[count].iov_len = sizeof(struct icmp6_hdr);
            } else {
                cmsg->cmsg_level = IPPROTO_IP;
                cmsg->cmsg_type = IP_TTL;
                cmsg->cmsg_len = CMSG_LEN(sizeof(int));
                *(int *)CMSG_DATA(cmsg) = intent.ttl;
                struct icmphdr *h = (struct icmphdr *)pkts[count];
                h->type = ICMP_ECHO; h->code = 0; h->un.echo.id = htons(g_gk_id);
                h->un.echo.sequence = htons(intent.seq); h->checksum = 0;
                h->checksum = ttak_net_icmp_calculate_checksum(h, sizeof(*h));
                iovs[count].iov_len = sizeof(struct icmphdr);
            }
            count++;
        }
        if (__builtin_expect(count > 0, 1)) sendmmsg(fd, msgs, count, 0);

        // [2] RECV OBSERVATIONS
        uint8_t buf[2048];
        struct sockaddr_storage from;
        socklen_t flen = sizeof(from);
        while (1) {
            ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
            if (n <= 0) break;
            
            uint16_t rid = 0, rseq = 0;
            nsr_obs_type_t type = NSR_OBS_NONE;
            if (!is_v6) {
                struct iphdr *ip = (struct iphdr *)buf;
                struct icmphdr *icmp = (struct icmphdr *)(buf + (ip->ihl * 4));
                if (icmp->type == ICMP_ECHOREPLY) {
                    rid = ntohs(icmp->un.echo.id); rseq = ntohs(icmp->un.echo.sequence); type = NSR_OBS_REPLY;
                } else if (icmp->type == ICMP_TIME_EXCEEDED) {
                    struct iphdr *iip = (struct iphdr *)(buf + (ip->ihl * 4) + 8);
                    struct icmphdr *iic = (struct icmphdr *)(buf + (ip->ihl * 4) + 8 + (iip->ihl * 4));
                    rid = ntohs(iic->un.echo.id); rseq = ntohs(iic->un.echo.sequence); type = NSR_OBS_EXCEEDED;
                }
            } else {
                struct icmp6_hdr *icmp6 = (struct icmp6_hdr *)buf;
                if (icmp6->icmp6_type == ICMP6_ECHO_REPLY) {
                    rid = ntohs(icmp6->icmp6_id); rseq = ntohs(icmp6->icmp6_seq); type = NSR_OBS_REPLY;
                } else if (icmp6->icmp6_type == ICMP6_TIME_EXCEEDED) {
                    struct icmp6_hdr *iic6 = (struct icmp6_hdr *)(buf + sizeof(struct icmp6_hdr) + sizeof(struct ip6_hdr));
                    rid = ntohs(iic6->icmp6_id); rseq = ntohs(iic6->icmp6_seq); type = NSR_OBS_EXCEEDED;
                }
            }

            if (type != NSR_OBS_NONE && rid == g_gk_id) {
                uint8_t ttl; uint64_t sts;
                if (find_probe(rseq, &ttl, &sts)) {
                    nsr_observation_t obs = {.ttl = ttl, .seq = rseq, .type = type, .rtt_us = (ttak_get_tick_count_ns()/1000) - sts};
                    if (is_v6) inet_ntop(AF_INET6, &((struct sockaddr_in6*)&from)->sin6_addr, obs.addr, 48);
                    else inet_ntop(AF_INET, &((struct sockaddr_in*)&from)->sin_addr, obs.addr, 48);
                    while(!nsr_shm_ring_push(g2l, &obs, sizeof(obs)));
                }
            }
        }
    }
}
