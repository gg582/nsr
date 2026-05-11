/**
 * @file omni_gatekeeper.c
 * @brief Ultra-High-Performance ICMP Gatekeeper for NSR.
 */

#define _GNU_SOURCE
#include <nsr/omni.h>
#include <ttak/net/session.h>
#include <ttak/timing/timing.h>
#include <ttak/net/core/icmp.h>
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
#include <fcntl.h>

typedef struct {
    uint16_t seq;
    uint8_t  ttl;
    uint64_t sent_us;
    bool     active;
} nsr_inflight_t;

static nsr_inflight_t g_inflight[NSR_GK_MAX_INFLIGHT] __attribute__((aligned(64)));
static uint16_t g_gk_id = 0;

static inline void track_probe(uint8_t ttl, uint16_t seq, uint64_t now_us) {
    int idx = seq % NSR_GK_MAX_INFLIGHT;
    g_inflight[idx].ttl = ttl;
    g_inflight[idx].seq = seq;
    g_inflight[idx].sent_us = now_us;
    g_inflight[idx].active = true;
}

static inline bool find_probe(uint16_t seq, uint8_t *ttl, uint64_t *sent_us) {
    int idx = seq % NSR_GK_MAX_INFLIGHT;
    if (g_inflight[idx].active && g_inflight[idx].seq == seq) {
        *ttl = g_inflight[idx].ttl;
        *sent_us = g_inflight[idx].sent_us;
        g_inflight[idx].active = false;
        return true;
    }
    return false;
}

static inline uint16_t fast_icmp_checksum(uint16_t id, uint16_t seq) {
    uint32_t sum = 0x0800 + id + seq; // 0x0800 is ICMP_ECHO type (8) and code (0)
    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    return ~((uint16_t)sum);
}

void nsr_omni_gatekeeper_omega(nsr_shm_ring_t *l2g, nsr_shm_ring_t *g2l, const char *target_ip) {
    bool is_v6 = (strchr(target_ip, ':') != NULL);
    int domain = is_v6 ? AF_INET6 : AF_INET;
    int proto = is_v6 ? IPPROTO_ICMPV6 : IPPROTO_ICMP;

    g_gk_id = (uint16_t)getpid();
    int fd = socket(domain, SOCK_RAW, proto);
    if (fd < 0) { perror("socket"); exit(1); }
    fcntl(fd, F_SETFL, O_NONBLOCK);

    int buf_size = 32 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));

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

    #define BATCH_SIZE 1024
    struct mmsghdr msgs[BATCH_SIZE];
    struct iovec iovs[BATCH_SIZE];
    uint8_t pkts[BATCH_SIZE][128];
    uint8_t controls[BATCH_SIZE][CMSG_SPACE(sizeof(int))];

    memset(msgs, 0, sizeof(msgs));
    uint16_t id_net = htons(g_gk_id);
    for (int i = 0; i < BATCH_SIZE; i++) {
        iovs[i].iov_base = pkts[i];
        msgs[i].msg_hdr.msg_iov = &iovs[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
        msgs[i].msg_hdr.msg_name = &dest_addr;
        msgs[i].msg_hdr.msg_namelen = is_v6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in);
        msgs[i].msg_hdr.msg_control = controls[i];
        msgs[i].msg_hdr.msg_controllen = sizeof(controls[i]);
        
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msgs[i].msg_hdr);
        cmsg->cmsg_level = is_v6 ? IPPROTO_IPV6 : IPPROTO_IP;
        cmsg->cmsg_type = is_v6 ? IPV6_HOPLIMIT : IP_TTL;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));

        if (!is_v6) {
            struct icmphdr *h = (struct icmphdr *)pkts[i];
            h->type = ICMP_ECHO; h->code = 0; h->un.echo.id = id_net;
            iovs[i].iov_len = sizeof(struct icmphdr);
        } else {
            struct icmp6_hdr *h = (struct icmp6_hdr *)pkts[i];
            h->icmp6_type = ICMP6_ECHO_REQUEST; h->icmp6_code = 0;
            h->icmp6_id = id_net;
            iovs[i].iov_len = sizeof(struct icmp6_hdr);
        }
    }

    struct mmsghdr rmsgs[BATCH_SIZE];
    struct iovec riovs[BATCH_SIZE];
    uint8_t rpkts[BATCH_SIZE][512];
    struct sockaddr_storage rfroms[BATCH_SIZE];

    memset(rmsgs, 0, sizeof(rmsgs));
    for (int i = 0; i < BATCH_SIZE; i++) {
        riovs[i].iov_base = rpkts[i];
        riovs[i].iov_len = sizeof(rpkts[i]);
        rmsgs[i].msg_hdr.msg_iov = &riovs[i];
        rmsgs[i].msg_hdr.msg_iovlen = 1;
        rmsgs[i].msg_hdr.msg_name = &rfroms[i];
        rmsgs[i].msg_hdr.msg_namelen = sizeof(struct sockaddr_storage);
    }

    nsr_observation_t batch_obs[BATCH_SIZE];
    nsr_intent_t intents[BATCH_SIZE];

    while (1) {
        // [1] PULL INTENTS & BATCH SEND
        int n_intents = nsr_shm_ring_pop_batch(l2g, intents, BATCH_SIZE, sizeof(nsr_intent_t));
        uint64_t now = ttak_get_tick_count_ns() / 1000;
        
        for (int i = 0; i < n_intents; i++) {
            nsr_intent_t *intent = &intents[i];
            track_probe(intent->ttl, intent->seq, now);
            struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msgs[i].msg_hdr);
            *(int *)CMSG_DATA(cmsg) = intent->ttl;

            if (is_v6) {
                struct icmp6_hdr *h = (struct icmp6_hdr *)pkts[i];
                h->icmp6_seq = htons(intent->seq);
                h->icmp6_cksum = 0; // Kernel/NIC handles this
            } else {
                struct icmphdr *h = (struct icmphdr *)pkts[i];
                uint16_t seq_net = htons(intent->seq);
                h->un.echo.sequence = seq_net;
                h->checksum = fast_icmp_checksum(id_net, seq_net);
            }
        }
        if (n_intents > 0) sendmmsg(fd, msgs, n_intents, 0);

        // [2] RECV OBSERVATIONS (Drain)
        while (1) {
            int rcount = recvmmsg(fd, rmsgs, BATCH_SIZE, MSG_DONTWAIT, NULL);
            if (rcount <= 0) break;
            
            int obs_count = 0;
            uint64_t now_us = ttak_get_tick_count_ns() / 1000;

            for (int i = 0; i < rcount; i++) {
                uint8_t *buf = rpkts[i];
                ssize_t n = rmsgs[i].msg_len;
                if (n <= 0) continue;

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
                        batch_obs[obs_count].ttl = ttl;
                        batch_obs[obs_count].seq = rseq;
                        batch_obs[obs_count].type = type;
                        batch_obs[obs_count].rtt_us = now_us - sts;
                        batch_obs[obs_count].is_v6 = is_v6;
                        if (is_v6) memcpy(batch_obs[obs_count].addr_bin, &((struct sockaddr_in6*)&rfroms[i])->sin6_addr, 16);
                        else memcpy(batch_obs[obs_count].addr_bin, &((struct sockaddr_in*)&rfroms[i])->sin_addr, 4);
                        obs_count++;
                    }
                }
            }
            if (obs_count > 0) {
                while(!nsr_shm_ring_push_batch(g2l, batch_obs, obs_count, sizeof(nsr_observation_t)));
            }
        }
    }
}
