/**
 * @file gatekeeper.c
 * @brief High-performance ICMP gatekeeper for NSR.
 *
 * Responsibilities:
 *   - Raw socket ownership (IPv4 ICMP / IPv6 ICMPv6).
 *   - Batched Tx via sendmmsg(2).
 *   - Batched Rx via recvmmsg(2) with strict boundary validation.
 *   - ICMP Type 11 (Time Exceeded) and Type 3 (Destination Unreachable)
 *     correlation back to the originating probe sequence.
 *   - Stale inflight reaper to prevent sequence-wrap collisions.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <nsr/telemetry.h>
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
#include <stdalign.h>
#include <stddef.h>

typedef struct {
    uint16_t seq;
    uint8_t  ttl;
    uint64_t sent_us;
    bool     active;
} nsr_inflight_t;

/* Cache-line aligned to avoid false sharing with recvmmsg path. */
static alignas(64) nsr_inflight_t g_inflight[NSR_GK_MAX_INFLIGHT];
static uint16_t g_gk_id = 0;

static inline void track_probe(uint8_t ttl, uint16_t seq, uint64_t now_us)
{
    int idx = seq % NSR_GK_MAX_INFLIGHT;
    g_inflight[idx].ttl     = ttl;
    g_inflight[idx].seq     = seq;
    g_inflight[idx].sent_us = now_us;
    g_inflight[idx].active  = true;
}

static inline bool find_probe(uint16_t seq, uint8_t *ttl, uint64_t *sent_us)
{
    int idx = seq % NSR_GK_MAX_INFLIGHT;
    if (g_inflight[idx].active && g_inflight[idx].seq == seq) {
        *ttl     = g_inflight[idx].ttl;
        *sent_us = g_inflight[idx].sent_us;
        g_inflight[idx].active = false;
        return true;
    }
    return false;
}

static inline void drop_probe(uint16_t seq)
{
    int idx = seq % NSR_GK_MAX_INFLIGHT;
    if (g_inflight[idx].active && g_inflight[idx].seq == seq)
        g_inflight[idx].active = false;
}

/* Reap entries older than 5 s to prevent seq-wrap false positives. */
static inline void reap_stale_inflight(uint64_t now_us)
{
    static uint64_t last_reap = 0;
    if (__builtin_expect(now_us - last_reap < 1000000ULL, 1))
        return;
    last_reap = now_us;

    for (int i = 0; i < NSR_GK_MAX_INFLIGHT; i++) {
        if (g_inflight[i].active && (now_us - g_inflight[i].sent_us) > 5000000ULL)
            g_inflight[i].active = false;
    }
}

/* Fast checksum for a minimal 8-byte ICMP Echo Request (type=8, code=0). */
static inline uint16_t fast_icmp_checksum(uint16_t id, uint16_t seq)
{
    uint32_t sum = 0x0800U + id + seq;
    sum = (sum >> 16) + (sum & 0xFFFFU);
    sum += (sum >> 16);
    return (uint16_t)~sum;
}

static bool parse_ipv6_original_icmp6(const uint8_t *buf, ssize_t n,
                                      size_t ip6_off, uint16_t *rid,
                                      uint16_t *rseq)
{
    if (n < (ssize_t)(ip6_off + sizeof(struct ip6_hdr)))
        return false;

    const struct ip6_hdr *ip6 = (const struct ip6_hdr *)(buf + ip6_off);
    uint8_t next = ip6->ip6_nxt;
    size_t off = ip6_off + sizeof(struct ip6_hdr);
    size_t end = ip6_off + sizeof(struct ip6_hdr) + ntohs(ip6->ip6_plen);
    if (end > (size_t)n)
        end = (size_t)n;

    while (1) {
        switch (next) {
        case IPPROTO_HOPOPTS:
        case IPPROTO_ROUTING:
        case IPPROTO_DSTOPTS: {
            if (off + 2 > end)
                return false;
            next = buf[off];
            size_t hdr_len = ((size_t)buf[off + 1] + 1U) * 8U;
            if (hdr_len == 0 || off + hdr_len > end)
                return false;
            off += hdr_len;
            break;
        }
        case IPPROTO_FRAGMENT:
            if (off + 8 > end)
                return false;
            next = buf[off];
            off += 8;
            break;
        case IPPROTO_AH: {
            if (off + 2 > end)
                return false;
            next = buf[off];
            size_t hdr_len = ((size_t)buf[off + 1] + 2U) * 4U;
            if (hdr_len == 0 || off + hdr_len > end)
                return false;
            off += hdr_len;
            break;
        }
        case IPPROTO_ICMPV6: {
            if (off + sizeof(struct icmp6_hdr) > end)
                return false;
            const struct icmp6_hdr *icmp6 = (const struct icmp6_hdr *)(buf + off);
            if (icmp6->icmp6_type != ICMP6_ECHO_REQUEST)
                return false;
            *rid = ntohs(icmp6->icmp6_id);
            *rseq = ntohs(icmp6->icmp6_seq);
            return true;
        }
        default:
            return false;
        }
    }
}

void nsr_gatekeeper_run(nsr_shm_ring_t *l2g, nsr_shm_ring_t *g2l, const char *target_ip)
{
    struct in6_addr target6;
    struct in_addr target4;
    bool is_v6 = inet_pton(AF_INET6, target_ip, &target6) == 1;
    bool is_v4 = !is_v6 && inet_pton(AF_INET, target_ip, &target4) == 1;
    if (!is_v6 && !is_v4) {
        fprintf(stderr, "gatekeeper: invalid target address: %s\n", target_ip);
        exit(1);
    }

    int domain = is_v6 ? AF_INET6 : AF_INET;
    int proto  = is_v6 ? IPPROTO_ICMPV6 : IPPROTO_ICMP;

    g_gk_id = (uint16_t)getpid();
    int fd = socket(domain, SOCK_RAW, proto);
    if (fd < 0) {
        perror("socket");
        exit(1);
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);

    int buf_size = 32 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
    if (is_v6) {
        int checksum_offset = offsetof(struct icmp6_hdr, icmp6_cksum);
        if (setsockopt(fd, IPPROTO_IPV6, IPV6_CHECKSUM,
                       &checksum_offset, sizeof(checksum_offset)) < 0) {
            /*
             * Linux computes ICMPv6 checksums for IPPROTO_ICMPV6 raw sockets.
             * Keep running if the explicit checksum offset is rejected.
             */
            if (errno != EINVAL && errno != ENOPROTOOPT)
                perror("setsockopt(IPV6_CHECKSUM)");
        }
    }

    struct sockaddr_storage dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    if (is_v6) {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)&dest_addr;
        a->sin6_family = AF_INET6;
        a->sin6_addr = target6;
    } else {
        struct sockaddr_in *a = (struct sockaddr_in *)&dest_addr;
        a->sin_family = AF_INET;
        a->sin_addr = target4;
    }

#define BATCH_SIZE 1024
    struct mmsghdr msgs[BATCH_SIZE];
    struct iovec   iovs[BATCH_SIZE];
    alignas(16) uint8_t pkts[BATCH_SIZE][128];
    alignas(16) uint8_t controls[BATCH_SIZE][CMSG_SPACE(sizeof(int))];

    memset(msgs, 0, sizeof(msgs));
    uint16_t id_net = htons(g_gk_id);
    for (int i = 0; i < BATCH_SIZE; i++) {
        iovs[i].iov_base = pkts[i];
        msgs[i].msg_hdr.msg_iov        = &iovs[i];
        msgs[i].msg_hdr.msg_iovlen     = 1;
        msgs[i].msg_hdr.msg_name       = &dest_addr;
        msgs[i].msg_hdr.msg_namelen    = is_v6 ? sizeof(struct sockaddr_in6)
                                               : sizeof(struct sockaddr_in);
        msgs[i].msg_hdr.msg_control    = controls[i];
        msgs[i].msg_hdr.msg_controllen = sizeof(controls[i]);

        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msgs[i].msg_hdr);
        cmsg->cmsg_level = is_v6 ? IPPROTO_IPV6 : IPPROTO_IP;
        cmsg->cmsg_type  = is_v6 ? IPV6_HOPLIMIT : IP_TTL;
        cmsg->cmsg_len   = CMSG_LEN(sizeof(int));

        if (!is_v6) {
            struct icmphdr *h = (struct icmphdr *)pkts[i];
            h->type          = ICMP_ECHO;
            h->code          = 0;
            h->un.echo.id    = id_net;
            iovs[i].iov_len  = sizeof(struct icmphdr);
        } else {
            struct icmp6_hdr *h = (struct icmp6_hdr *)pkts[i];
            h->icmp6_type     = ICMP6_ECHO_REQUEST;
            h->icmp6_code     = 0;
            h->icmp6_id       = id_net;
            iovs[i].iov_len   = sizeof(struct icmp6_hdr);
        }
    }

    struct mmsghdr rmsgs[BATCH_SIZE];
    struct iovec   riovs[BATCH_SIZE];
    alignas(16) uint8_t rpkts[BATCH_SIZE][512];
    struct sockaddr_storage rfroms[BATCH_SIZE];

    memset(rmsgs, 0, sizeof(rmsgs));
    for (int i = 0; i < BATCH_SIZE; i++) {
        riovs[i].iov_base = rpkts[i];
        riovs[i].iov_len  = sizeof(rpkts[i]);
        rmsgs[i].msg_hdr.msg_iov       = &riovs[i];
        rmsgs[i].msg_hdr.msg_iovlen    = 1;
        rmsgs[i].msg_hdr.msg_name      = &rfroms[i];
        rmsgs[i].msg_hdr.msg_namelen   = sizeof(struct sockaddr_storage);
    }

    nsr_observation_t batch_obs[BATCH_SIZE];
    nsr_intent_t      intents[BATCH_SIZE];

    while (1) {
        int n_intents = nsr_shm_ring_pop_batch(l2g, intents, BATCH_SIZE, sizeof(nsr_intent_t));
        uint64_t now = ttak_get_tick_count_ns() / 1000;
        reap_stale_inflight(now);

        for (int i = 0; i < n_intents; i++) {
            nsr_intent_t *intent = &intents[i];
            track_probe(intent->ttl, intent->seq, now);
            struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msgs[i].msg_hdr);
            *(int *)CMSG_DATA(cmsg) = intent->ttl;

            if (is_v6) {
                struct icmp6_hdr *h = (struct icmp6_hdr *)pkts[i];
                h->icmp6_seq   = htons(intent->seq);
                h->icmp6_cksum = 0;
            } else {
                struct icmphdr *h = (struct icmphdr *)pkts[i];
                uint16_t seq_net = htons(intent->seq);
                h->un.echo.sequence = seq_net;
                h->checksum         = fast_icmp_checksum(id_net, seq_net);
            }
        }
        if (n_intents > 0) {
            int sent = sendmmsg(fd, msgs, n_intents, 0);
            if (sent < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                    perror("sendmmsg");
                for (int i = 0; i < n_intents; i++)
                    drop_probe(intents[i].seq);
            } else if (sent < n_intents) {
                for (int i = sent; i < n_intents; i++)
                    drop_probe(intents[i].seq);
            }
        }

        while (1) {
            for (int i = 0; i < BATCH_SIZE; i++)
                rmsgs[i].msg_hdr.msg_namelen = sizeof(struct sockaddr_storage);
            int rcount = recvmmsg(fd, rmsgs, BATCH_SIZE, MSG_DONTWAIT, NULL);
            if (rcount <= 0)
                break;

            int obs_count = 0;
            uint64_t now_us = ttak_get_tick_count_ns() / 1000;

            for (int i = 0; i < rcount; i++) {
                uint8_t *buf = rpkts[i];
                ssize_t  n   = rmsgs[i].msg_len;
                if (n <= 0)
                    continue;

                uint16_t rid  = 0;
                uint16_t rseq = 0;
                nsr_obs_type_t type = NSR_OBS_NONE;

                if (!is_v6) {
                    if (n < (ssize_t)sizeof(struct iphdr))
                        continue;
                    struct iphdr *ip = (struct iphdr *)buf;
                    if (ip->version != 4)
                        continue;
                    size_t ip_hdr_len = (size_t)ip->ihl * 4U;
                    if (ip->ihl < 5 || n < (ssize_t)(ip_hdr_len + sizeof(struct icmphdr)))
                        continue;

                    struct icmphdr *icmp = (struct icmphdr *)(buf + ip_hdr_len);

                    if (icmp->type == ICMP_ECHOREPLY) {
                        rid  = ntohs(icmp->un.echo.id);
                        rseq = ntohs(icmp->un.echo.sequence);
                        type = NSR_OBS_REPLY;
                    } else if (icmp->type == ICMP_TIME_EXCEEDED ||
                               icmp->type == ICMP_DEST_UNREACH) {
                        size_t inner_off = ip_hdr_len + 8U;
                        if (n < (ssize_t)(inner_off + sizeof(struct iphdr) + sizeof(struct icmphdr)))
                            continue;
                        struct iphdr *iip = (struct iphdr *)(buf + inner_off);
                        size_t iip_len = (size_t)iip->ihl * 4U;
                        if (iip->ihl < 5 || n < (ssize_t)(inner_off + iip_len + sizeof(struct icmphdr)))
                            continue;
                        struct icmphdr *iic = (struct icmphdr *)(buf + inner_off + iip_len);
                        rid  = ntohs(iic->un.echo.id);
                        rseq = ntohs(iic->un.echo.sequence);
                        type = (icmp->type == ICMP_DEST_UNREACH) ? NSR_OBS_UNREACH
                                                                  : NSR_OBS_EXCEEDED;
                    }
                } else {
                    if (n < (ssize_t)sizeof(struct icmp6_hdr))
                        continue;
                    struct icmp6_hdr *icmp6 = (struct icmp6_hdr *)buf;

                    if (icmp6->icmp6_type == ICMP6_ECHO_REPLY) {
                        rid  = ntohs(icmp6->icmp6_id);
                        rseq = ntohs(icmp6->icmp6_seq);
                        type = NSR_OBS_REPLY;
                    } else if (icmp6->icmp6_type == ICMP6_TIME_EXCEEDED ||
                               icmp6->icmp6_type == ICMP6_DST_UNREACH) {
                        if (!parse_ipv6_original_icmp6(buf, n, sizeof(struct icmp6_hdr),
                                                       &rid, &rseq))
                            continue;
                        type = (icmp6->icmp6_type == ICMP6_DST_UNREACH) ? NSR_OBS_UNREACH
                                                                        : NSR_OBS_EXCEEDED;
                    }
                }

                if (type != NSR_OBS_NONE && rid == g_gk_id) {
                    uint8_t  ttl;
                    uint64_t sts;
                    if (find_probe(rseq, &ttl, &sts)) {
                        batch_obs[obs_count].ttl     = ttl;
                        batch_obs[obs_count].seq     = rseq;
                        batch_obs[obs_count].type    = type;
                        batch_obs[obs_count].rtt_us  = now_us - sts;
                        batch_obs[obs_count].is_v6   = is_v6;
                        if (is_v6)
                            memcpy(batch_obs[obs_count].addr_bin,
                                   &((struct sockaddr_in6 *)&rfroms[i])->sin6_addr, 16);
                        else
                            memcpy(batch_obs[obs_count].addr_bin,
                                   &((struct sockaddr_in *)&rfroms[i])->sin_addr, 4);
                        obs_count++;
                    }
                }
            }
            if (obs_count > 0) {
                while (!nsr_shm_ring_push_batch(g2l, batch_obs, obs_count, sizeof(nsr_observation_t)))
                    ;
            }
        }
    }
}
