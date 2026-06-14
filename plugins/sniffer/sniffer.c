#include <nsr/json/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <netinet/if_ether.h>
#include <net/ethernet.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <curl/curl.h>
#include <sys/types.h>
#include <netdb.h>

#define SNIFF_CAPTURE_MAX 128
#define SNIFF_TARGET_MAX  48
#define SNIFF_INFO_MAX    128
#define SNIFF_IFNAME_MAX  16

typedef enum {
    DPI_PROTO_OTHER = 0,
    DPI_PROTO_ICMP,
    DPI_PROTO_TCP,
    DPI_PROTO_UDP,
} dpi_trans_proto_t;

typedef enum {
    DPI_APP_UNKNOWN = 0,
    DPI_APP_HTTP,
    DPI_APP_HTTPS,
    DPI_APP_DNS,
    DPI_APP_SSH,
    DPI_APP_FTP,
    DPI_APP_SMTP,
    DPI_APP_TLS,
} dpi_app_proto_t;

typedef struct {
    uint64_t ts_ms;
    char src_ip[INET6_ADDRSTRLEN];
    char dst_ip[INET6_ADDRSTRLEN];
    uint16_t src_port;
    uint16_t dst_port;
    dpi_trans_proto_t trans;
    dpi_app_proto_t app;
    uint8_t tcp_flags;
    uint32_t payload_len;
    uint32_t ip_total_len;
    char info[SNIFF_INFO_MAX];
} sniff_packet_t;

typedef struct {
    char target_ip[SNIFF_TARGET_MAX];
    char target_host[SNIFF_TARGET_MAX];
    char telemetry_target_ip[SNIFF_TARGET_MAX];
    bool is_ipv6;
    bool running;

    int sock;
    pthread_t thread;
    pthread_mutex_t lock;

    sniff_packet_t packets[SNIFF_CAPTURE_MAX];
    uint32_t head;
    uint32_t count;
    uint64_t total_captured;
    uint64_t total_dropped;
    uint64_t total_filtered;

    char focused_addr[SNIFF_TARGET_MAX];
    bool error_no_priv;

    /* Filtering options */
    char filter_proto[32];
    char filter_string[64];
} sniff_state_t;

static sniff_state_t g_state;

static const char *app_proto_name(dpi_app_proto_t app);
static const char *trans_proto_name(dpi_trans_proto_t trans);
static const char *tcp_flags_str(uint8_t flags, char *out, size_t out_len);

/* Inspection Dashboard Variables */
static bool g_show_dashboard = false;
static char g_inspect_ip[SNIFF_TARGET_MAX] = "";
static int g_inspect_proto_idx = 0;
static char g_inspect_filter[64] = "";
static int g_inspect_focus = 0;
static int g_enter_press_count = 0;
static uint64_t g_last_a_press_ms = 0;
static bool g_countdown_active = false;
static uint64_t g_countdown_start_ms = 0;
static bool g_inspect_show_raw = false;
static int g_inspect_body_mode = 0; // 0: UTF-8, 1: Hex

static bool g_state_show_raw = false;
static int g_state_body_mode = 0; // 0: UTF-8, 1: Hex

#define SNIFF_PAYLOAD_MAX 65536
static uint8_t g_last_payload[SNIFF_PAYLOAD_MAX];
static uint32_t g_last_payload_len = 0;

static void write_hex_dump(FILE *f, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i += 16) {
        fprintf(f, "%08x: ", i);
        for (int j = 0; j < 16; j++) {
            if (i + j < len) {
                fprintf(f, "%02x ", data[i + j]);
            } else {
                fprintf(f, "   ");
            }
        }
        fprintf(f, " ");
        for (int j = 0; j < 16; j++) {
            if (i + j < len) {
                char c = (char)data[i + j];
                fprintf(f, "%c", isprint((int)c) ? c : '.');
            }
        }
        fprintf(f, "\n");
    }
}

static const char *PROTO_FILTERS[] = {
    "ALL", "TCP", "UDP", "ICMP", "HTTP", "HTTPS", "DNS", "SSH", "FTP", "SMTP", "TLS"
};

#ifndef KEY_UP
#define KEY_UP 259
#endif
#ifndef KEY_DOWN
#define KEY_DOWN 258
#endif
#ifndef KEY_LEFT
#define KEY_LEFT 260
#endif
#ifndef KEY_RIGHT
#define KEY_RIGHT 261
#endif
#ifndef KEY_BACKSPACE
#define KEY_BACKSPACE 263
#endif
#ifndef KEY_ENTER
#define KEY_ENTER 343
#endif

static sniff_packet_t g_last_packet;
static bool g_has_last_packet = false;

static bool write_placeholder_packet_file(void)
{
    FILE *f = fopen("/tmp/nsr_last_packet.txt", "w");
    if (!f) return false;
    fprintf(f, "==================================================\n");
    fprintf(f, "               NSR PACKET INSPECTION              \n");
    fprintf(f, "==================================================\n\n");
    fprintf(f, "No packet captured yet.\n\n");
    fprintf(f, "Instructions:\n");
    fprintf(f, "1. Focus a hop in the TUI normal mode.\n");
    fprintf(f, "2. Press [f] to start sniffing that hop.\n");
    fprintf(f, "3. Double-tap [a] to configure packet & payload filters.\n");
    fprintf(f, "==================================================\n");
    fclose(f);
    return true;
}

static void send_editor_response(long long id, const char *filepath)
{
    nsr_json_buf_t resp;
    nsr_json_init(&resp);
    nsr_json_obj_start(&resp);
    nsr_json_key(&resp, "jsonrpc");
    nsr_json_string(&resp, "2.0");
    nsr_json_key(&resp, "id");
    nsr_json_int(&resp, id);
    nsr_json_key(&resp, "result");
    nsr_json_obj_start(&resp);
    nsr_json_key(&resp, "handled");
    nsr_json_bool(&resp, true);
    nsr_json_key(&resp, "action");
    nsr_json_string(&resp, "open_editor");
    nsr_json_key(&resp, "file");
    nsr_json_string(&resp, filepath);
    nsr_json_obj_end(&resp);
    nsr_json_obj_end(&resp);
    printf("%s\n", nsr_json_cstr(&resp));
    fflush(stdout);
    nsr_json_free(&resp);
}

static bool packet_matches_filters(const sniff_packet_t *pkt, const uint8_t *payload, uint32_t payload_len)
{
    /* 1. Protocol Filter */
    if (g_state.filter_proto[0] && strcmp(g_state.filter_proto, "ALL") != 0) {
        bool match = false;
        
        if (strcasecmp(g_state.filter_proto, "TCP") == 0 && pkt->trans == DPI_PROTO_TCP) match = true;
        else if (strcasecmp(g_state.filter_proto, "UDP") == 0 && pkt->trans == DPI_PROTO_UDP) match = true;
        else if (strcasecmp(g_state.filter_proto, "ICMP") == 0 && pkt->trans == DPI_PROTO_ICMP) match = true;
        
        else if (strcasecmp(g_state.filter_proto, "HTTP") == 0 && pkt->app == DPI_APP_HTTP) match = true;
        else if (strcasecmp(g_state.filter_proto, "HTTPS") == 0 && pkt->app == DPI_APP_HTTPS) match = true;
        else if (strcasecmp(g_state.filter_proto, "DNS") == 0 && pkt->app == DPI_APP_DNS) match = true;
        else if (strcasecmp(g_state.filter_proto, "SSH") == 0 && pkt->app == DPI_APP_SSH) match = true;
        else if (strcasecmp(g_state.filter_proto, "FTP") == 0 && pkt->app == DPI_APP_FTP) match = true;
        else if (strcasecmp(g_state.filter_proto, "SMTP") == 0 && pkt->app == DPI_APP_SMTP) match = true;
        else if (strcasecmp(g_state.filter_proto, "TLS") == 0 && pkt->app == DPI_APP_TLS) match = true;
        
        if (!match) return false;
    }
    
    /* 2. String Filter */
    if (g_state.filter_string[0]) {
        bool match = false;
        
        if (payload_len > 0 && payload) {
            char *temp = malloc(payload_len + 1);
            if (temp) {
                memcpy(temp, payload, payload_len);
                temp[payload_len] = '\0';
                if (strcasestr(temp, g_state.filter_string) != NULL) {
                    match = true;
                }
                free(temp);
            }
        }
        
        if (!match && strcasestr(pkt->info, g_state.filter_string) != NULL) {
            match = true;
        }
        
        if (!match) return false;
    }
    
    return true;
}

static bool write_last_packet_to_file(const sniff_packet_t *pkt)
{
    FILE *f = fopen("/tmp/nsr_last_packet.txt", "w");
    if (!f) return false;

    char time_str[64];
    time_t sec = pkt->ts_ms / 1000;
    struct tm *tm_info = localtime(&sec);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);

    fprintf(f, "==================================================\n");
    fprintf(f, "               NSR PACKET INSPECTION              \n");
    fprintf(f, "==================================================\n\n");
    fprintf(f, "Timestamp:        %s.%03d\n", time_str, (int)(pkt->ts_ms % 1000));
    fprintf(f, "Source IP:        %s\n", pkt->src_ip);
    fprintf(f, "Destination IP:   %s\n", pkt->dst_ip);
    fprintf(f, "Transport Proto:  %s\n", trans_proto_name(pkt->trans));
    if (pkt->trans == DPI_PROTO_TCP || pkt->trans == DPI_PROTO_UDP) {
        fprintf(f, "Source Port:      %u\n", pkt->src_port);
        fprintf(f, "Destination Port: %u\n", pkt->dst_port);
    }
    if (pkt->trans == DPI_PROTO_TCP) {
        char flags_buf[32];
        fprintf(f, "TCP Flags:        %s\n", tcp_flags_str(pkt->tcp_flags, flags_buf, sizeof(flags_buf)));
    }
    fprintf(f, "App Protocol:     %s\n", app_proto_name(pkt->app));
    fprintf(f, "IP Total Length:  %u bytes\n", pkt->ip_total_len);
    fprintf(f, "Payload Length:   %u bytes\n\n", pkt->payload_len);
    fprintf(f, "Packet Summary:\n%s\n\n", pkt->info);
    fprintf(f, "==================================================\n");
    fclose(f);
    return true;
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static const char *app_proto_name(dpi_app_proto_t app)
{
    switch (app) {
    case DPI_APP_HTTP:   return "HTTP";
    case DPI_APP_HTTPS:  return "HTTPS";
    case DPI_APP_DNS:    return "DNS";
    case DPI_APP_SSH:    return "SSH";
    case DPI_APP_FTP:    return "FTP";
    case DPI_APP_SMTP:   return "SMTP";
    case DPI_APP_TLS:    return "TLS";
    default:             return "?";
    }
}

static const char *trans_proto_name(dpi_trans_proto_t trans)
{
    switch (trans) {
    case DPI_PROTO_ICMP: return "ICMP";
    case DPI_PROTO_TCP:  return "TCP";
    case DPI_PROTO_UDP:  return "UDP";
    default:             return "OTHER";
    }
}

static const char *tcp_flags_str(uint8_t flags, char *out, size_t out_len)
{
    char buf[8];
    size_t n = 0;
    if (flags & TH_FIN) buf[n++] = 'F';
    if (flags & TH_SYN) buf[n++] = 'S';
    if (flags & TH_RST) buf[n++] = 'R';
    if (flags & TH_PUSH) buf[n++] = 'P';
    if (flags & TH_ACK) buf[n++] = 'A';
    if (flags & TH_URG) buf[n++] = 'U';
    buf[n] = '\0';
    snprintf(out, out_len, "[%s]", n ? buf : "-");
    return out;
}

static bool ip_is_loopback(const char *ip)
{
    return strcmp(ip, "127.0.0.1") == 0 || strcmp(ip, "::1") == 0;
}

static bool ip_matches_target(const char *ip)
{
    if (!g_state.target_ip[0])
        return false;
    return strcmp(ip, g_state.target_ip) == 0;
}

static dpi_app_proto_t detect_app_by_ports(uint16_t sport, uint16_t dport)
{
    uint16_t ports[2] = { sport, dport };
    for (int i = 0; i < 2; i++) {
        switch (ports[i]) {
        case 20: case 21: return DPI_APP_FTP;
        case 22: return DPI_APP_SSH;
        case 25: case 587: return DPI_APP_SMTP;
        case 53: return DPI_APP_DNS;
        case 80: return DPI_APP_HTTP;
        case 443: return DPI_APP_HTTPS;
        }
    }
    return DPI_APP_UNKNOWN;
}

static bool payload_looks_like_tls(const uint8_t *payload, uint32_t len)
{
    if (len < 3)
        return false;
    return payload[0] == 0x16 && payload[1] == 0x03 && payload[2] <= 0x04;
}

static bool payload_looks_like_http(const uint8_t *payload, uint32_t len)
{
    if (len < 8)
        return false;
    const char *p = (const char *)payload;
    return strncmp(p, "GET ", 4) == 0 ||
           strncmp(p, "POST ", 5) == 0 ||
           strncmp(p, "PUT ", 4) == 0 ||
           strncmp(p, "HTTP/", 5) == 0 ||
           strncmp(p, "HEAD ", 5) == 0;
}

static bool payload_looks_like_dns(const uint8_t *payload, uint32_t len)
{
    if (len < 12)
        return false;
    uint16_t txid = (uint16_t)payload[0] << 8 | payload[1];
    (void)txid;
    uint16_t flags = (uint16_t)payload[2] << 8 | payload[3];
    uint16_t opcode = (flags >> 11) & 0x0f;
    return opcode <= 5;
}

static bool payload_looks_like_ssh(const uint8_t *payload, uint32_t len)
{
    if (len < 7)
        return false;
    return strncmp((const char *)payload, "SSH-2.0", 7) == 0 ||
           strncmp((const char *)payload, "SSH-1.99", 8) == 0;
}

static dpi_app_proto_t refine_app_proto(dpi_app_proto_t app,
                                        const uint8_t *payload, uint32_t len)
{
    if (len == 0)
        return app;

    if (app == DPI_APP_HTTPS && payload_looks_like_tls(payload, len))
        return DPI_APP_TLS;

    if (app == DPI_APP_UNKNOWN) {
        if (payload_looks_like_http(payload, len))
            return DPI_APP_HTTP;
        if (payload_looks_like_ssh(payload, len))
            return DPI_APP_SSH;
        if (payload_looks_like_tls(payload, len))
            return DPI_APP_TLS;
        if (payload_looks_like_dns(payload, len))
            return DPI_APP_DNS;
    }

    return app;
}

static bool parse_http_summary(const uint8_t *payload, uint32_t len,
                               char *out, size_t out_len)
{
    if (len < 8)
        return false;

    const char *p = (const char *)payload;
    const char *end = p + len;
    const char *line_end = memchr(p, '\n', len);
    if (!line_end)
        line_end = end;

    /* Request: METHOD /path HTTP/1.x */
    if (strncmp(p, "GET ", 4) == 0 || strncmp(p, "POST ", 5) == 0 ||
        strncmp(p, "PUT ", 4) == 0 || strncmp(p, "DELETE ", 7) == 0 ||
        strncmp(p, "HEAD ", 5) == 0 || strncmp(p, "OPTIONS ", 8) == 0 ||
        strncmp(p, "PATCH ", 6) == 0) {
        const char *m = p;
        const char *sp1 = memchr(m, ' ', line_end - m);
        if (!sp1) return false;
        const char *sp2 = memchr(sp1 + 1, ' ', line_end - sp1 - 1);
        if (!sp2) sp2 = line_end;

        char method[16] = "";
        size_t mlen = sp1 - m;
        if (mlen >= sizeof(method)) mlen = sizeof(method) - 1;
        memcpy(method, m, mlen);
        method[mlen] = '\0';

        char path[64] = "";
        size_t plen = sp2 - sp1 - 1;
        if (plen >= sizeof(path)) plen = sizeof(path) - 1;
        memcpy(path, sp1 + 1, plen);
        path[plen] = '\0';

        const char *host = "";
        char host_buf[64] = "";
        const char *hp = strcasestr((const char *)payload, "Host: ");
        if (hp) {
            hp += 6;
            const char *he = memchr(hp, '\r', end - hp);
            if (!he) he = memchr(hp, '\n', end - hp);
            if (!he) he = end;
            size_t hlen = he - hp;
            if (hlen >= sizeof(host_buf)) hlen = sizeof(host_buf) - 1;
            memcpy(host_buf, hp, hlen);
            host_buf[hlen] = '\0';
            host = host_buf;
        }

        if (host[0])
            snprintf(out, out_len, "%s %s (%s)", method, path, host);
        else
            snprintf(out, out_len, "%s %s", method, path);
        return true;
    }

    /* Response: HTTP/1.x CODE ... */
    if (strncmp(p, "HTTP/", 5) == 0) {
        const char *sp = memchr(p + 5, ' ', line_end - p - 5);
        if (sp && sp + 1 < line_end) {
            char code[8] = "";
            size_t clen = line_end - sp - 1;
            if (clen >= sizeof(code)) clen = sizeof(code) - 1;
            memcpy(code, sp + 1, clen);
            code[clen] = '\0';
            snprintf(out, out_len, "RESP %s", code);
            return true;
        }
    }

    return false;
}

static uint16_t read_u16be(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static uint32_t read_u24be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static bool parse_tls_client_hello(const uint8_t *payload, uint32_t len,
                                   char *sni_out, size_t sni_out_len,
                                   bool *ech_out)
{
    *sni_out = '\0';
    *ech_out = false;

    if (len < 43)
        return false;
    if (payload[0] != 0x16) /* not handshake */
        return false;

    uint32_t rec_len = read_u16be(payload + 3);
    if (rec_len + 5 > len)
        return false;
    if (payload[5] != 0x01) /* not ClientHello */
        return false;

    uint32_t hs_len = read_u24be(payload + 6);
    if (hs_len + 9 > len)
        return false;

    const uint8_t *p = payload + 9;
    const uint8_t *end = p + hs_len;
    if (end > payload + len)
        return false;

    /* skip client version(2) + random(32) */
    p += 34;
    if (p + 1 > end) return false;
    uint8_t sid_len = *p++;
    p += sid_len;
    if (p + 2 > end) return false;
    uint16_t cipher_len = read_u16be(p);
    p += 2 + cipher_len;
    if (p + 1 > end) return false;
    uint8_t comp_len = *p++;
    p += comp_len;
    if (p + 2 > end) return false;
    uint16_t ext_len = read_u16be(p);
    p += 2;
    const uint8_t *ext_end = p + ext_len;
    if (ext_end > end) ext_end = end;

    while (p + 4 <= ext_end) {
        uint16_t ext_type = read_u16be(p);
        uint16_t ext_data_len = read_u16be(p + 2);
        p += 4;
        if (p + ext_data_len > ext_end)
            break;

        if (ext_type == 0x0000) { /* SNI */
            const uint8_t *sn = p;
            if (ext_data_len < 5) { p += ext_data_len; continue; }
            uint16_t sni_list_len = read_u16be(sn);
            if (sni_list_len + 2 > ext_data_len) { p += ext_data_len; continue; }
            sn += 2;
            uint8_t name_type = *sn++;
            if (name_type != 0) { p += ext_data_len; continue; }
            uint16_t name_len = read_u16be(sn);
            sn += 2;
            if (sn + name_len > p + ext_data_len) { p += ext_data_len; continue; }
            size_t nlen = name_len;
            if (nlen >= sni_out_len) nlen = sni_out_len - 1;
            memcpy(sni_out, sn, nlen);
            sni_out[nlen] = '\0';
        } else if (ext_type == 0xfe0d || (ext_type >= 0xff03 && ext_type <= 0xff09)) {
            /* ECH: final (0xfe0d) or draft versions (0xff03-0xff09) */
            *ech_out = true;
        }

        p += ext_data_len;
    }

    return true;
}

static void build_info(sniff_packet_t *pkt, uint32_t len, const uint8_t *payload)
{
    char tflags[16] = "";
    const char *app = app_proto_name(pkt->app);

    /* Try HTTP/TLS deep inspection. */
    if (pkt->trans == DPI_PROTO_TCP && len > 0 && payload) {
        char detail[128] = "";

        if (pkt->app == DPI_APP_HTTP || pkt->app == DPI_APP_HTTPS ||
            payload_looks_like_http(payload, len)) {
            if (parse_http_summary(payload, len, detail, sizeof(detail))) {
                tcp_flags_str(pkt->tcp_flags, tflags, sizeof(tflags));
                if (pkt->src_port && pkt->dst_port) {
                    snprintf(pkt->info, sizeof(pkt->info), "%s %s %u->%u %s",
                             app, tflags, pkt->src_port, pkt->dst_port, detail);
                } else {
                    snprintf(pkt->info, sizeof(pkt->info), "%s %s %s",
                             app, tflags, detail);
                }
                return;
            }
        }

        if (pkt->app == DPI_APP_TLS || pkt->app == DPI_APP_HTTPS ||
            payload_looks_like_tls(payload, len)) {
            char sni[96] = "";
            bool ech = false;
            if (parse_tls_client_hello(payload, len, sni, sizeof(sni), &ech)) {
                tcp_flags_str(pkt->tcp_flags, tflags, sizeof(tflags));
                if (sni[0]) {
                    snprintf(pkt->info, sizeof(pkt->info), "%s %s %u->%u SNI=%s%s",
                             app, tflags, pkt->src_port, pkt->dst_port,
                             sni, ech ? " [ECH]" : "");
                } else {
                    snprintf(pkt->info, sizeof(pkt->info), "%s %s %u->%u TLS%s",
                             app, tflags, pkt->src_port, pkt->dst_port,
                             ech ? " [ECH]" : "");
                }
                return;
            }
        }
    }

    if (pkt->trans == DPI_PROTO_TCP) {
        tcp_flags_str(pkt->tcp_flags, tflags, sizeof(tflags));
        if (pkt->src_port && pkt->dst_port) {
            snprintf(pkt->info, sizeof(pkt->info), "%s %s %u->%u len=%u",
                     app, tflags, pkt->src_port, pkt->dst_port, len);
        } else {
            snprintf(pkt->info, sizeof(pkt->info), "%s %s len=%u", app, tflags, len);
        }
    } else if (pkt->trans == DPI_PROTO_UDP) {
        if (pkt->src_port && pkt->dst_port) {
            snprintf(pkt->info, sizeof(pkt->info), "%s %u->%u len=%u",
                     app, pkt->src_port, pkt->dst_port, len);
        } else {
            snprintf(pkt->info, sizeof(pkt->info), "%s len=%u", app, len);
        }
    } else {
        snprintf(pkt->info, sizeof(pkt->info), "%s len=%u", trans_proto_name(pkt->trans), len);
    }
}

static void analyze_ipv4(const uint8_t *data, uint32_t cap_len)
{
    if (cap_len < sizeof(struct iphdr))
        return;

    const struct iphdr *ip = (const struct iphdr *)data;
    uint32_t ihl = ip->ihl * 4;
    if (ihl < sizeof(struct iphdr) || cap_len < ihl)
        return;

    char src_ip[INET_ADDRSTRLEN], dst_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &ip->saddr, src_ip, sizeof(src_ip));
    inet_ntop(AF_INET, &ip->daddr, dst_ip, sizeof(dst_ip));

    if (!ip_matches_target(src_ip) && !ip_matches_target(dst_ip)) {
        __atomic_add_fetch(&g_state.total_filtered, 1, __ATOMIC_RELAXED);
        return;
    }

    sniff_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.ts_ms = now_ms();
    strncpy(pkt.src_ip, src_ip, sizeof(pkt.src_ip) - 1);
    strncpy(pkt.dst_ip, dst_ip, sizeof(pkt.dst_ip) - 1);
    pkt.ip_total_len = ntohs(ip->tot_len);
    pkt.trans = DPI_PROTO_OTHER;
    pkt.app = DPI_APP_UNKNOWN;

    const uint8_t *trans_data = data + ihl;
    uint32_t trans_len = cap_len - ihl;
    const uint8_t *payload = NULL;
    uint32_t payload_len = 0;

    switch (ip->protocol) {
    case IPPROTO_TCP: {
        if (trans_len < sizeof(struct tcphdr))
            return;
        const struct tcphdr *tcp = (const struct tcphdr *)trans_data;
        pkt.trans = DPI_PROTO_TCP;
        pkt.src_port = ntohs(tcp->source);
        pkt.dst_port = ntohs(tcp->dest);
        pkt.tcp_flags = tcp->th_flags;
        uint32_t thl = tcp->doff * 4;
        if (thl < sizeof(struct tcphdr) || thl > trans_len)
            return;
        payload = trans_data + thl;
        payload_len = trans_len - thl;
        pkt.app = detect_app_by_ports(pkt.src_port, pkt.dst_port);
        break;
    }
    case IPPROTO_UDP: {
        if (trans_len < sizeof(struct udphdr))
            return;
        const struct udphdr *udp = (const struct udphdr *)trans_data;
        pkt.trans = DPI_PROTO_UDP;
        pkt.src_port = ntohs(udp->source);
        pkt.dst_port = ntohs(udp->dest);
        payload = trans_data + sizeof(struct udphdr);
        payload_len = trans_len - sizeof(struct udphdr);
        pkt.app = detect_app_by_ports(pkt.src_port, pkt.dst_port);
        break;
    }
    case IPPROTO_ICMP:
        pkt.trans = DPI_PROTO_ICMP;
        payload = trans_data;
        payload_len = trans_len;
        break;
    default:
        payload = trans_data;
        payload_len = trans_len;
        break;
    }

    if (payload_len > ntohs(ip->tot_len) - ihl)
        payload_len = ntohs(ip->tot_len) - ihl;

    pkt.app = refine_app_proto(pkt.app, payload, payload_len);
    pkt.payload_len = payload_len;
    build_info(&pkt, payload_len, payload);

    if (!packet_matches_filters(&pkt, payload, payload_len)) {
        __atomic_add_fetch(&g_state.total_filtered, 1, __ATOMIC_RELAXED);
        return;
    }

    pthread_mutex_lock(&g_state.lock);
    g_state.packets[g_state.head] = pkt;
    g_state.head = (g_state.head + 1) % SNIFF_CAPTURE_MAX;
    if (g_state.count < SNIFF_CAPTURE_MAX)
        g_state.count++;
    g_last_packet = pkt;
    g_has_last_packet = true;
    if (payload && payload_len > 0) {
        uint32_t cpy_len = payload_len < SNIFF_PAYLOAD_MAX ? payload_len : SNIFF_PAYLOAD_MAX;
        memcpy(g_last_payload, payload, cpy_len);
        g_last_payload_len = cpy_len;
    }
    pthread_mutex_unlock(&g_state.lock);

    __atomic_add_fetch(&g_state.total_captured, 1, __ATOMIC_RELAXED);
}

static void analyze_ipv6(const uint8_t *data, uint32_t cap_len)
{
    if (cap_len < sizeof(struct ip6_hdr))
        return;

    const struct ip6_hdr *ip6 = (const struct ip6_hdr *)data;

    char src_ip[INET6_ADDRSTRLEN], dst_ip[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &ip6->ip6_src, src_ip, sizeof(src_ip));
    inet_ntop(AF_INET6, &ip6->ip6_dst, dst_ip, sizeof(dst_ip));

    if (!ip_matches_target(src_ip) && !ip_matches_target(dst_ip)) {
        __atomic_add_fetch(&g_state.total_filtered, 1, __ATOMIC_RELAXED);
        return;
    }

    sniff_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.ts_ms = now_ms();
    strncpy(pkt.src_ip, src_ip, sizeof(pkt.src_ip) - 1);
    strncpy(pkt.dst_ip, dst_ip, sizeof(pkt.dst_ip) - 1);
    pkt.ip_total_len = sizeof(struct ip6_hdr) + ntohs(ip6->ip6_plen);
    pkt.trans = DPI_PROTO_OTHER;
    pkt.app = DPI_APP_UNKNOWN;

    uint8_t nxt = ip6->ip6_nxt;
    const uint8_t *trans_data = data + sizeof(struct ip6_hdr);
    uint32_t trans_len = cap_len - sizeof(struct ip6_hdr);
    const uint8_t *payload = NULL;
    uint32_t payload_len = 0;

    switch (nxt) {
    case IPPROTO_TCP: {
        if (trans_len < sizeof(struct tcphdr))
            return;
        const struct tcphdr *tcp = (const struct tcphdr *)trans_data;
        pkt.trans = DPI_PROTO_TCP;
        pkt.src_port = ntohs(tcp->source);
        pkt.dst_port = ntohs(tcp->dest);
        pkt.tcp_flags = tcp->th_flags;
        uint32_t thl = tcp->doff * 4;
        if (thl < sizeof(struct tcphdr) || thl > trans_len)
            return;
        payload = trans_data + thl;
        payload_len = trans_len - thl;
        pkt.app = detect_app_by_ports(pkt.src_port, pkt.dst_port);
        break;
    }
    case IPPROTO_UDP: {
        if (trans_len < sizeof(struct udphdr))
            return;
        const struct udphdr *udp = (const struct udphdr *)trans_data;
        pkt.trans = DPI_PROTO_UDP;
        pkt.src_port = ntohs(udp->source);
        pkt.dst_port = ntohs(udp->dest);
        payload = trans_data + sizeof(struct udphdr);
        payload_len = trans_len - sizeof(struct udphdr);
        pkt.app = detect_app_by_ports(pkt.src_port, pkt.dst_port);
        break;
    }
    case IPPROTO_ICMPV6:
        pkt.trans = DPI_PROTO_ICMP;
        payload = trans_data;
        payload_len = trans_len;
        break;
    default:
        payload = trans_data;
        payload_len = trans_len;
        break;
    }

    if (payload_len > ntohs(ip6->ip6_plen))
        payload_len = ntohs(ip6->ip6_plen);

    pkt.app = refine_app_proto(pkt.app, payload, payload_len);
    pkt.payload_len = payload_len;
    build_info(&pkt, payload_len, payload);

    if (!packet_matches_filters(&pkt, payload, payload_len)) {
        __atomic_add_fetch(&g_state.total_filtered, 1, __ATOMIC_RELAXED);
        return;
    }

    pthread_mutex_lock(&g_state.lock);
    g_state.packets[g_state.head] = pkt;
    g_state.head = (g_state.head + 1) % SNIFF_CAPTURE_MAX;
    if (g_state.count < SNIFF_CAPTURE_MAX)
        g_state.count++;
    g_last_packet = pkt;
    g_has_last_packet = true;
    if (payload && payload_len > 0) {
        uint32_t cpy_len = payload_len < SNIFF_PAYLOAD_MAX ? payload_len : SNIFF_PAYLOAD_MAX;
        memcpy(g_last_payload, payload, cpy_len);
        g_last_payload_len = cpy_len;
    }
    pthread_mutex_unlock(&g_state.lock);

    __atomic_add_fetch(&g_state.total_captured, 1, __ATOMIC_RELAXED);
}

static void *capture_thread(void *arg)
{
    (void)arg;
    uint8_t buf[65536];

    while (g_state.running) {
        ssize_t n = recv(g_state.sock, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            usleep(10000);
            continue;
        }
        if (n < (ssize_t)sizeof(struct ether_header))
            continue;

        const struct ether_header *eth = (const struct ether_header *)buf;
        uint16_t eth_type = ntohs(eth->ether_type);
        const uint8_t *l3 = buf + sizeof(struct ether_header);
        uint32_t l3_len = (uint32_t)(n - sizeof(struct ether_header));

        if (eth_type == ETHERTYPE_IP) {
            analyze_ipv4(l3, l3_len);
        } else if (eth_type == ETHERTYPE_IPV6) {
            analyze_ipv6(l3, l3_len);
        }
    }

    return NULL;
}

static bool sniffer_resolve_host(const char *hostname, char *ip_out, size_t ip_out_len)
{
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(hostname, NULL, &hints, &res) != 0) {
        return false;
    }

    bool success = false;
    struct addrinfo *p;
    for (p = res; p != NULL; p = p->ai_next) {
        if (p->ai_family == AF_INET) {
            struct sockaddr_in *ipv4 = (struct sockaddr_in *)p->ai_addr;
            inet_ntop(AF_INET, &(ipv4->sin_addr), ip_out, ip_out_len);
            success = true;
            break;
        } else if (p->ai_family == AF_INET6) {
            struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)p->ai_addr;
            inet_ntop(AF_INET6, &(ipv6->sin6_addr), ip_out, ip_out_len);
            success = true;
            break;
        }
    }
    freeaddrinfo(res);
    return success;
}

static void stop_sniffer(void);

static bool start_sniffer(const char *target_ip)
{
    g_state.error_no_priv = false;

    if (!target_ip || !target_ip[0] || ip_is_loopback(target_ip))
        return false;

    char resolved_ip[SNIFF_TARGET_MAX];
    char target_host[SNIFF_TARGET_MAX];
    strncpy(target_host, target_ip, sizeof(target_host) - 1);
    target_host[sizeof(target_host) - 1] = '\0';

    struct in_addr ipv4_addr;
    struct in6_addr ipv6_addr;
    if (inet_pton(AF_INET, target_ip, &ipv4_addr) == 1 || inet_pton(AF_INET6, target_ip, &ipv6_addr) == 1) {
        strncpy(resolved_ip, target_ip, sizeof(resolved_ip) - 1);
        resolved_ip[sizeof(resolved_ip) - 1] = '\0';
        if (strcmp(target_ip, g_state.telemetry_target_ip) == 0 && g_state.target_host[0]) {
            strncpy(target_host, g_state.target_host, sizeof(target_host) - 1);
            target_host[sizeof(target_host) - 1] = '\0';
        }
    } else {
        if (!sniffer_resolve_host(target_ip, resolved_ip, sizeof(resolved_ip))) {
            return false;
        }
    }

    if (g_state.running && strcmp(g_state.target_ip, resolved_ip) == 0) {
        strncpy(g_state.target_host, target_host, sizeof(g_state.target_host) - 1);
        g_state.target_host[sizeof(g_state.target_host) - 1] = '\0';
        return true;
    }

    stop_sniffer();

    int sock = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (sock < 0)
        return false;

    struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    strncpy(g_state.target_ip, resolved_ip, sizeof(g_state.target_ip) - 1);
    g_state.target_ip[sizeof(g_state.target_ip) - 1] = '\0';
    strncpy(g_state.target_host, target_host, sizeof(g_state.target_host) - 1);
    g_state.target_host[sizeof(g_state.target_host) - 1] = '\0';
    g_state.is_ipv6 = strchr(resolved_ip, ':') != NULL;
    g_state.sock = sock;
    g_state.running = true;
    g_state.total_captured = 0;
    g_state.total_filtered = 0;

    pthread_mutex_lock(&g_state.lock);
    g_state.head = 0;
    g_state.count = 0;
    pthread_mutex_unlock(&g_state.lock);

    if (pthread_create(&g_state.thread, NULL, capture_thread, NULL) != 0) {
        g_state.running = false;
        close(g_state.sock);
        g_state.sock = -1;
        return false;
    }

    return true;
}

static void stop_sniffer(void)
{
    if (!g_state.running)
        return;

    g_state.running = false;
    pthread_join(g_state.thread, NULL);

    if (g_state.sock >= 0) {
        close(g_state.sock);
        g_state.sock = -1;
    }

    g_state.target_ip[0] = '\0';
    g_state.target_host[0] = '\0';
}

static void send_bool_response(long long id, const char *key, bool val)
{
    nsr_json_buf_t resp;
    nsr_json_init(&resp);
    nsr_json_obj_start(&resp);
    nsr_json_key(&resp, "jsonrpc");
    nsr_json_string(&resp, "2.0");
    nsr_json_key(&resp, "id");
    nsr_json_int(&resp, id);
    nsr_json_key(&resp, "result");
    nsr_json_obj_start(&resp);
    nsr_json_key(&resp, key);
    nsr_json_bool(&resp, val);
    nsr_json_obj_end(&resp);
    nsr_json_obj_end(&resp);
    printf("%s\n", nsr_json_cstr(&resp));
    fflush(stdout);
    nsr_json_free(&resp);
}

static void handle_init(const char *params, long long id)
{
    (void)params;

    nsr_json_buf_t resp;
    nsr_json_init(&resp);
    nsr_json_obj_start(&resp);
    nsr_json_key(&resp, "jsonrpc");
    nsr_json_string(&resp, "2.0");
    nsr_json_key(&resp, "id");
    nsr_json_int(&resp, id);
    nsr_json_key(&resp, "result");
    nsr_json_obj_start(&resp);
    nsr_json_key(&resp, "status");
    nsr_json_string(&resp, "ok");
    nsr_json_key(&resp, "description");
    nsr_json_string(&resp, "Lightweight DPI packet sniffer (press f on a hop)");
    nsr_json_key(&resp, "reserved_keys");
    nsr_json_string(&resp, "fas");
    nsr_json_obj_end(&resp);
    nsr_json_obj_end(&resp);
    printf("%s\n", nsr_json_cstr(&resp));
    fflush(stdout);
    nsr_json_free(&resp);
}

static void handle_update_telemetry(const char *params)
{
    size_t len;
    const char *target = nsr_json_obj_get(params, "target_ip", &len);
    if (target) {
        nsr_json_parse_str(target, len, g_state.telemetry_target_ip, sizeof(g_state.telemetry_target_ip));
    }
    const char *host = nsr_json_obj_get(params, "target_host", &len);
    if (host) {
        nsr_json_parse_str(host, len, g_state.target_host, sizeof(g_state.target_host));
    }
}

static void *perform_curl_request_thread(void *arg)
{
    char *url = (char *)arg;
    CURL *curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_NOBODY, 0L); // GET request to fetch body
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L); // Follow redirects
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L); // Insecure
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L); // 5s timeout
        curl_easy_perform(curl);
        curl_easy_cleanup(curl);
    }
    free(url);
    return NULL;
}

static void handle_render(const char *params, long long id)
{
    size_t flen;
    const char *v = nsr_json_obj_get(params, "focused_addr", &flen);
    if (v) {
        nsr_json_parse_str(v, flen, g_state.focused_addr, sizeof(g_state.focused_addr));
    } else {
        g_state.focused_addr[0] = '\0';
    }

    long long width = 60;
    v = nsr_json_obj_get(params, "width", &flen);
    if (v) nsr_json_parse_int(v, flen, &width);
    if (width < 20) width = 60;

    long long height = 18;
    v = nsr_json_obj_get(params, "height", &flen);
    if (v) nsr_json_parse_int(v, flen, &height);
    if (height < 6) height = 6;

    if (g_countdown_active) {
        uint64_t elapsed = now_ms() - g_countdown_start_ms;
        if (elapsed >= 3000) {
            char host_buf[128];
            if (g_state.target_host[0]) {
                if (strchr(g_state.target_host, ':') != NULL) {
                    snprintf(host_buf, sizeof(host_buf), "[%s]", g_state.target_host);
                } else {
                    snprintf(host_buf, sizeof(host_buf), "%s", g_state.target_host);
                }
            } else {
                if (strchr(g_state.target_ip, ':') != NULL) {
                    snprintf(host_buf, sizeof(host_buf), "[%s]", g_state.target_ip);
                } else {
                    snprintf(host_buf, sizeof(host_buf), "%s", g_state.target_ip);
                }
            }
            char *url = malloc(256);
            if (url) {
                snprintf(url, 256, "%s://%s",
                         (strcasecmp(g_state.filter_proto, "HTTP") == 0) ? "http" : "https",
                         host_buf);
                pthread_t t;
                if (pthread_create(&t, NULL, perform_curl_request_thread, url) == 0) {
                    pthread_detach(t);
                } else {
                    free(url);
                }
            }
            g_countdown_active = false;
        } else {
            int rem = 3 - (int)(elapsed / 1000);
            if (rem < 1) rem = 1;
            
            nsr_json_buf_t resp;
            nsr_json_init(&resp);
            nsr_json_obj_start(&resp);
            nsr_json_key(&resp, "jsonrpc");
            nsr_json_string(&resp, "2.0");
            nsr_json_key(&resp, "id");
            nsr_json_int(&resp, id);
            nsr_json_key(&resp, "result");
            nsr_json_obj_start(&resp);
            
            nsr_json_key(&resp, "is_modal");
            nsr_json_bool(&resp, true);
            nsr_json_key(&resp, "modal_width");
            nsr_json_int(&resp, 60);
            nsr_json_key(&resp, "modal_height");
            nsr_json_int(&resp, 5);

            nsr_json_key(&resp, "lines");
            nsr_json_arr_start(&resp);
            
            char msg[128];
            snprintf(msg, sizeof(msg), "CURL Request Accepted. Starting in %ds...", rem);
            
            nsr_json_obj_start(&resp);
            nsr_json_key(&resp, "y");
            nsr_json_int(&resp, 2);
            nsr_json_key(&resp, "x");
            long long x_pos = (60 - (long long)strlen(msg)) / 2;
            if (x_pos < 1) x_pos = 1;
            nsr_json_int(&resp, x_pos);
            nsr_json_key(&resp, "text");
            nsr_json_string(&resp, msg);
            nsr_json_key(&resp, "color");
            nsr_json_string(&resp, "yellow");
            nsr_json_obj_end(&resp);
            
            nsr_json_arr_end(&resp);
            nsr_json_obj_end(&resp);
            nsr_json_obj_end(&resp);
            printf("%s\n", nsr_json_cstr(&resp));
            fflush(stdout);
            nsr_json_free(&resp);
            return;
        }
    }

    if (g_show_dashboard) {
        nsr_json_buf_t resp;
        nsr_json_init(&resp);
        nsr_json_obj_start(&resp);
        nsr_json_key(&resp, "jsonrpc");
        nsr_json_string(&resp, "2.0");
        nsr_json_key(&resp, "id");
        nsr_json_int(&resp, id);
        nsr_json_key(&resp, "result");
        nsr_json_obj_start(&resp);
        
        nsr_json_key(&resp, "is_modal");
        nsr_json_bool(&resp, true);
        nsr_json_key(&resp, "modal_width");
        nsr_json_int(&resp, 60);
        nsr_json_key(&resp, "modal_height");
        nsr_json_int(&resp, 15);
        
        nsr_json_key(&resp, "lines");
        nsr_json_arr_start(&resp);
        
        int y = 1;
        char line_buf[256];
        
        // 1. Title
        nsr_json_obj_start(&resp);
        nsr_json_key(&resp, "y");
        nsr_json_int(&resp, y++);
        nsr_json_key(&resp, "x");
        nsr_json_int(&resp, 12);
        nsr_json_key(&resp, "text");
        nsr_json_string(&resp, "=== PACKET INSPECTION SETTINGS ===");
        nsr_json_key(&resp, "color");
        nsr_json_string(&resp, "cyan");
        nsr_json_obj_end(&resp);
        
        y++; // empty line
        
        // 2. Target IP field
        snprintf(line_buf, sizeof(line_buf), "%s Target IP:       %s%s",
                 (g_inspect_focus == 0) ? " >" : "  ",
                 g_inspect_ip,
                 (g_inspect_focus == 0) ? "_" : "");
        nsr_json_obj_start(&resp);
        nsr_json_key(&resp, "y");
        nsr_json_int(&resp, y++);
        nsr_json_key(&resp, "x");
        nsr_json_int(&resp, 2);
        nsr_json_key(&resp, "text");
        nsr_json_string(&resp, line_buf);
        if (g_inspect_focus == 0) {
            nsr_json_key(&resp, "color");
            nsr_json_string(&resp, "yellow");
        }
        nsr_json_obj_end(&resp);
        
        // 3. Protocol filter field
        snprintf(line_buf, sizeof(line_buf), "%s Protocol Filter: < %s >",
                 (g_inspect_focus == 1) ? " >" : "  ",
                 PROTO_FILTERS[g_inspect_proto_idx]);
        nsr_json_obj_start(&resp);
        nsr_json_key(&resp, "y");
        nsr_json_int(&resp, y++);
        nsr_json_key(&resp, "x");
        nsr_json_int(&resp, 2);
        nsr_json_key(&resp, "text");
        nsr_json_string(&resp, line_buf);
        if (g_inspect_focus == 1) {
            nsr_json_key(&resp, "color");
            nsr_json_string(&resp, "yellow");
        }
        nsr_json_obj_end(&resp);
        
        // 4. String filter field
        snprintf(line_buf, sizeof(line_buf), "%s Payload Filter:  %s%s",
                 (g_inspect_focus == 2) ? " >" : "  ",
                 g_inspect_filter,
                 (g_inspect_focus == 2) ? "_" : "");
        nsr_json_obj_start(&resp);
        nsr_json_key(&resp, "y");
        nsr_json_int(&resp, y++);
        nsr_json_key(&resp, "x");
        nsr_json_int(&resp, 2);
        nsr_json_key(&resp, "text");
        nsr_json_string(&resp, line_buf);
        if (g_inspect_focus == 2) {
            nsr_json_key(&resp, "color");
            nsr_json_string(&resp, "yellow");
        }
        nsr_json_obj_end(&resp);

        // 5. Show Raw Body field
        snprintf(line_buf, sizeof(line_buf), "%s Show Raw Body:    [ %s ]",
                 (g_inspect_focus == 3) ? " >" : "  ",
                 g_inspect_show_raw ? "Yes" : "No ");
        nsr_json_obj_start(&resp);
        nsr_json_key(&resp, "y");
        nsr_json_int(&resp, y++);
        nsr_json_key(&resp, "x");
        nsr_json_int(&resp, 2);
        nsr_json_key(&resp, "text");
        nsr_json_string(&resp, line_buf);
        if (g_inspect_focus == 3) {
            nsr_json_key(&resp, "color");
            nsr_json_string(&resp, "yellow");
        }
        nsr_json_obj_end(&resp);
        
        // 6. Body Mode field
        snprintf(line_buf, sizeof(line_buf), "%s Body Mode:        < %s >",
                 (g_inspect_focus == 4) ? " >" : "  ",
                 (g_inspect_body_mode == 0) ? "UTF-8" : "Hex  ");
        nsr_json_obj_start(&resp);
        nsr_json_key(&resp, "y");
        nsr_json_int(&resp, y++);
        nsr_json_key(&resp, "x");
        nsr_json_int(&resp, 2);
        nsr_json_key(&resp, "text");
        nsr_json_string(&resp, line_buf);
        if (g_inspect_focus == 4) {
            nsr_json_key(&resp, "color");
            nsr_json_string(&resp, "yellow");
        }
        nsr_json_obj_end(&resp);
        
        y++; // empty line
        
        // 5. Help / Instructions
        nsr_json_obj_start(&resp);
        nsr_json_key(&resp, "y");
        nsr_json_int(&resp, y++);
        nsr_json_key(&resp, "x");
        nsr_json_int(&resp, 4);
        nsr_json_key(&resp, "text");
        nsr_json_string(&resp, "Use [Up/Down] to navigate fields");
        nsr_json_key(&resp, "color");
        nsr_json_string(&resp, "white");
        nsr_json_obj_end(&resp);
        
        nsr_json_obj_start(&resp);
        nsr_json_key(&resp, "y");
        nsr_json_int(&resp, y++);
        nsr_json_key(&resp, "x");
        nsr_json_int(&resp, 4);
        nsr_json_key(&resp, "text");
        nsr_json_string(&resp, "Use [Left/Right] to change Protocol/Raw/Mode");
        nsr_json_key(&resp, "color");
        nsr_json_string(&resp, "white");
        nsr_json_obj_end(&resp);

        nsr_json_obj_start(&resp);
        nsr_json_key(&resp, "y");
        nsr_json_int(&resp, y++);
        nsr_json_key(&resp, "x");
        nsr_json_int(&resp, 4);
        nsr_json_key(&resp, "text");
        if (g_enter_press_count == 1) {
            nsr_json_string(&resp, "Press ENTER once more to APPLY & CLOSE");
            nsr_json_key(&resp, "color");
            nsr_json_string(&resp, "green");
        } else {
            nsr_json_string(&resp, "Press ENTER twice to APPLY & CLOSE");
            nsr_json_key(&resp, "color");
            nsr_json_string(&resp, "white");
        }
        nsr_json_obj_end(&resp);

        nsr_json_obj_start(&resp);
        nsr_json_key(&resp, "y");
        nsr_json_int(&resp, y++);
        nsr_json_key(&resp, "x");
        nsr_json_int(&resp, 4);
        nsr_json_key(&resp, "text");
        nsr_json_string(&resp, "Press ESC to Cancel");
        nsr_json_key(&resp, "color");
        nsr_json_string(&resp, "red");
        nsr_json_obj_end(&resp);

        nsr_json_arr_end(&resp);
        nsr_json_obj_end(&resp);
        nsr_json_obj_end(&resp);
        printf("%s\n", nsr_json_cstr(&resp));
        fflush(stdout);
        nsr_json_free(&resp);
        return;
    }

    nsr_json_buf_t resp;
    nsr_json_init(&resp);
    nsr_json_obj_start(&resp);
    nsr_json_key(&resp, "jsonrpc");
    nsr_json_string(&resp, "2.0");
    nsr_json_key(&resp, "id");
    nsr_json_int(&resp, id);
    nsr_json_key(&resp, "result");
    nsr_json_obj_start(&resp);
    nsr_json_key(&resp, "lines");
    nsr_json_arr_start(&resp);

    int y = 1;
    char buf[256];

    /* Title */
    nsr_json_obj_start(&resp);
    nsr_json_key(&resp, "y");
    nsr_json_int(&resp, y++);
    nsr_json_key(&resp, "x");
    nsr_json_int(&resp, 1);
    nsr_json_key(&resp, "text");
    nsr_json_string(&resp, "== Lightweight DPI Sniffer ==");
    nsr_json_key(&resp, "color");
    nsr_json_string(&resp, "cyan");
    nsr_json_obj_end(&resp);

    /* Status line */
    if (g_state.running && g_state.target_ip[0]) {
        snprintf(buf, sizeof(buf), "Sniffing: %s", g_state.target_ip);
        nsr_json_obj_start(&resp);
        nsr_json_key(&resp, "y");
        nsr_json_int(&resp, y++);
        nsr_json_key(&resp, "x");
        nsr_json_int(&resp, 1);
        nsr_json_key(&resp, "text");
        nsr_json_string(&resp, buf);
        nsr_json_key(&resp, "color");
        nsr_json_string(&resp, "green");
        nsr_json_obj_end(&resp);

        uint64_t captured = __atomic_load_n(&g_state.total_captured, __ATOMIC_RELAXED);
        uint64_t filtered = __atomic_load_n(&g_state.total_filtered, __ATOMIC_RELAXED);
        snprintf(buf, sizeof(buf), "Captured: %llu  Filtered: %llu",
                 (unsigned long long)captured, (unsigned long long)filtered);
        nsr_json_obj_start(&resp);
        nsr_json_key(&resp, "y");
        nsr_json_int(&resp, y++);
        nsr_json_key(&resp, "x");
        nsr_json_int(&resp, 1);
        nsr_json_key(&resp, "text");
        nsr_json_string(&resp, buf);
        nsr_json_obj_end(&resp);
    } else {
        nsr_json_obj_start(&resp);
        nsr_json_key(&resp, "y");
        nsr_json_int(&resp, y++);
        nsr_json_key(&resp, "x");
        nsr_json_int(&resp, 1);
        nsr_json_key(&resp, "text");
        if (g_state.error_no_priv) {
            snprintf(buf, sizeof(buf), "Error: raw socket failed (need root/cap_net_raw)");
            nsr_json_string(&resp, buf);
            nsr_json_key(&resp, "color");
            nsr_json_string(&resp, "red");
        } else if (g_state.focused_addr[0]) {
            snprintf(buf, sizeof(buf), "Focus: %s  Press [f] to sniff", g_state.focused_addr);
            nsr_json_string(&resp, buf);
            nsr_json_key(&resp, "color");
            nsr_json_string(&resp, "yellow");
        } else {
            nsr_json_string(&resp, "Focus a hop, then press [f] to sniff");
            nsr_json_key(&resp, "color");
            nsr_json_string(&resp, "yellow");
        }
        nsr_json_obj_end(&resp);
    }

    y++;

    /* Recent packets */
    pthread_mutex_lock(&g_state.lock);
    if (g_state.count == 0) {
        nsr_json_obj_start(&resp);
        nsr_json_key(&resp, "y");
        nsr_json_int(&resp, y++);
        nsr_json_key(&resp, "x");
        nsr_json_int(&resp, 1);
        nsr_json_key(&resp, "text");
        nsr_json_string(&resp, "No packets yet...");
        nsr_json_key(&resp, "color");
        nsr_json_string(&resp, "white");
        nsr_json_obj_end(&resp);
    } else {
        uint32_t n = g_state.count < SNIFF_CAPTURE_MAX ? g_state.count : SNIFF_CAPTURE_MAX;
        for (uint32_t i = 0; i < n && y < height - 1; i++) {
            uint32_t idx = (g_state.head + SNIFF_CAPTURE_MAX - n + i) % SNIFF_CAPTURE_MAX;
            const sniff_packet_t *p = &g_state.packets[idx];

            char short_src[32], short_dst[32];
            strncpy(short_src, p->src_ip, sizeof(short_src) - 1);
            short_src[sizeof(short_src) - 1] = '\0';
            strncpy(short_dst, p->dst_ip, sizeof(short_dst) - 1);
            short_dst[sizeof(short_dst) - 1] = '\0';

            char line[256];
            snprintf(line, sizeof(line), "%s -> %s | %s",
                     short_src, short_dst, p->info);
            if ((int)strlen(line) > width - 2)
                line[width - 2] = '\0';

            nsr_json_obj_start(&resp);
            nsr_json_key(&resp, "y");
            nsr_json_int(&resp, y++);
            nsr_json_key(&resp, "x");
            nsr_json_int(&resp, 1);
            nsr_json_key(&resp, "text");
            nsr_json_string(&resp, line);
            nsr_json_key(&resp, "color");
            nsr_json_string(&resp, (p->trans == DPI_PROTO_TCP) ? "cyan" :
                                    (p->trans == DPI_PROTO_UDP) ? "magenta" : "white");
            nsr_json_obj_end(&resp);
        }
    }
    pthread_mutex_unlock(&g_state.lock);

    nsr_json_arr_end(&resp);
    nsr_json_obj_end(&resp);
    nsr_json_obj_end(&resp);
    printf("%s\n", nsr_json_cstr(&resp));
    fflush(stdout);
    nsr_json_free(&resp);
}

static void handle_on_key(const char *params, long long id)
{
    long long key = 0;
    size_t flen;
    const char *v = nsr_json_obj_get(params, "key", &flen);
    if (v) nsr_json_parse_int(v, flen, &key);

    if (g_show_dashboard) {
        bool handled = false;
        
        if (key == 27) { // ESC
            g_show_dashboard = false;
            g_enter_press_count = 0;
            handled = true;
        } else if (key == KEY_UP || key == 'k' || key == 'K') {
            g_inspect_focus = (g_inspect_focus + 4) % 5;
            g_enter_press_count = 0;
            handled = true;
        } else if (key == KEY_DOWN || key == 'j' || key == 'J') {
            g_inspect_focus = (g_inspect_focus + 1) % 5;
            g_enter_press_count = 0;
            handled = true;
        } else if (key == KEY_LEFT || key == 'h' || key == 'H') {
            if (g_inspect_focus == 1) {
                int count = sizeof(PROTO_FILTERS) / sizeof(PROTO_FILTERS[0]);
                g_inspect_proto_idx = (g_inspect_proto_idx + count - 1) % count;
            } else if (g_inspect_focus == 3) {
                g_inspect_show_raw = !g_inspect_show_raw;
            } else if (g_inspect_focus == 4) {
                g_inspect_body_mode = 1 - g_inspect_body_mode;
            }
            g_enter_press_count = 0;
            handled = true;
        } else if (key == KEY_RIGHT || key == 'l' || key == 'L') {
            if (g_inspect_focus == 1) {
                int count = sizeof(PROTO_FILTERS) / sizeof(PROTO_FILTERS[0]);
                g_inspect_proto_idx = (g_inspect_proto_idx + 1) % count;
            } else if (g_inspect_focus == 3) {
                g_inspect_show_raw = !g_inspect_show_raw;
            } else if (g_inspect_focus == 4) {
                g_inspect_body_mode = 1 - g_inspect_body_mode;
            }
            g_enter_press_count = 0;
            handled = true;
        } else if (key == KEY_ENTER || key == '\n' || key == '\r' || key == 10 || key == 13 || key == 343) {
            g_enter_press_count++;
            if (g_enter_press_count >= 2) {
                g_show_dashboard = false;
                g_enter_press_count = 0;
                
                strncpy(g_state.filter_proto, PROTO_FILTERS[g_inspect_proto_idx], sizeof(g_state.filter_proto) - 1);
                g_state.filter_proto[sizeof(g_state.filter_proto) - 1] = '\0';
                
                strncpy(g_state.filter_string, g_inspect_filter, sizeof(g_state.filter_string) - 1);
                g_state.filter_string[sizeof(g_state.filter_string) - 1] = '\0';
                
                g_state_show_raw = g_inspect_show_raw;
                g_state_body_mode = g_inspect_body_mode;

                if (g_inspect_ip[0]) {
                    start_sniffer(g_inspect_ip);
                } else {
                    stop_sniffer();
                }

                if (strcasecmp(g_state.filter_proto, "HTTP") == 0 ||
                    strcasecmp(g_state.filter_proto, "HTTPS") == 0 ||
                    strcasecmp(g_state.filter_proto, "TLS") == 0) {
                    g_countdown_active = true;
                    g_countdown_start_ms = now_ms();
                }
            }
            handled = true;
        } else if (key == KEY_BACKSPACE || key == 127 || key == 8) {
            g_enter_press_count = 0;
            if (g_inspect_focus == 0) {
                size_t len = strlen(g_inspect_ip);
                if (len > 0) {
                    g_inspect_ip[len - 1] = '\0';
                }
            } else if (g_inspect_focus == 2) {
                size_t len = strlen(g_inspect_filter);
                if (len > 0) {
                    g_inspect_filter[len - 1] = '\0';
                }
            }
            handled = true;
        } else if (isprint((int)key)) {
            g_enter_press_count = 0;
            if (g_inspect_focus == 0) {
                size_t len = strlen(g_inspect_ip);
                if (len < sizeof(g_inspect_ip) - 1) {
                    g_inspect_ip[len] = (char)key;
                    g_inspect_ip[len + 1] = '\0';
                }
            } else if (g_inspect_focus == 2) {
                size_t len = strlen(g_inspect_filter);
                if (len < sizeof(g_inspect_filter) - 1) {
                    g_inspect_filter[len] = (char)key;
                    g_inspect_filter[len + 1] = '\0';
                }
            }
            handled = true;
        }
        
        send_bool_response(id, "handled", handled);
        return;
    }

    if (key == 'a' || key == 'A') {
        uint64_t now = now_ms();
        if (now - g_last_a_press_ms < 500) {
            g_show_dashboard = true;
            g_inspect_focus = 0;
            g_enter_press_count = 0;
            if (g_inspect_ip[0] == '\0') {
                if (g_state.target_ip[0]) {
                    strncpy(g_inspect_ip, g_state.target_ip, sizeof(g_inspect_ip) - 1);
                } else if (g_state.focused_addr[0]) {
                    strncpy(g_inspect_ip, g_state.focused_addr, sizeof(g_inspect_ip) - 1);
                }
                g_inspect_ip[sizeof(g_inspect_ip) - 1] = '\0';
            }
            strncpy(g_inspect_filter, g_state.filter_string, sizeof(g_inspect_filter) - 1);
            g_inspect_filter[sizeof(g_inspect_filter) - 1] = '\0';
            
            g_inspect_show_raw = g_state_show_raw;
            g_inspect_body_mode = g_state_body_mode;

            g_inspect_proto_idx = 0;
            for (size_t i = 0; i < sizeof(PROTO_FILTERS)/sizeof(PROTO_FILTERS[0]); i++) {
                if (strcasecmp(PROTO_FILTERS[i], g_state.filter_proto) == 0) {
                    g_inspect_proto_idx = (int)i;
                    break;
                }
            }
        }
        g_last_a_press_ms = now;
        send_bool_response(id, "handled", true);
        return;
    }

    if (key == 's') {
        bool has_packet = false;
        sniff_packet_t last_pkt;
        
        pthread_mutex_lock(&g_state.lock);
        if (g_has_last_packet) {
            last_pkt = g_last_packet;
            has_packet = true;
            
            if (g_state_show_raw) {
                FILE *f = fopen("/tmp/nsr_last_packet.txt", "w");
                if (f) {
                    if (g_last_payload_len > 0) {
                        if (g_state_body_mode == 1) { // Hex
                            write_hex_dump(f, g_last_payload, g_last_payload_len);
                        } else { // UTF-8
                            fwrite(g_last_payload, 1, g_last_payload_len, f);
                        }
                    } else {
                        fprintf(f, "(No payload data captured)\n");
                    }
                    fclose(f);
                }
            }
        }
        pthread_mutex_unlock(&g_state.lock);
        
        if (has_packet) {
            if (!g_state_show_raw) {
                write_last_packet_to_file(&last_pkt);
            }
        } else {
            write_placeholder_packet_file();
        }
        
        send_editor_response(id, "/tmp/nsr_last_packet.txt");
        return;
    }

    if (key == 'f' || key == 'F') {
        if (g_state.focused_addr[0]) {
            bool started = start_sniffer(g_state.focused_addr);
            g_state.error_no_priv = !started;
            send_bool_response(id, "handled", true);
            return;
        }
    }

    send_bool_response(id, "handled", false);
}

static void handle_cleanup(const char *params)
{
    (void)params;
    stop_sniffer();
}

static void send_error(long long id, const char *message)
{
    nsr_json_buf_t resp;
    nsr_json_init(&resp);
    nsr_json_obj_start(&resp);
    nsr_json_key(&resp, "jsonrpc");
    nsr_json_string(&resp, "2.0");
    nsr_json_key(&resp, "id");
    nsr_json_int(&resp, id);
    nsr_json_key(&resp, "error");
    nsr_json_obj_start(&resp);
    nsr_json_key(&resp, "code");
    nsr_json_int(&resp, -32601);
    nsr_json_key(&resp, "message");
    nsr_json_string(&resp, message);
    nsr_json_obj_end(&resp);
    nsr_json_obj_end(&resp);
    printf("%s\n", nsr_json_cstr(&resp));
    fflush(stdout);
    nsr_json_free(&resp);
}

int main(void)
{
    curl_global_init(CURL_GLOBAL_ALL);
    memset(&g_state, 0, sizeof(g_state));
    pthread_mutex_init(&g_state.lock, NULL);
    g_state.sock = -1;

    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, stdin) != -1) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[--len] = '\0';
        if (len > 0 && line[len - 1] == '\r')
            line[--len] = '\0';

        size_t mlen;
        const char *method = nsr_json_obj_get(line, "method", &mlen);
        if (!method)
            continue;

        size_t plen;
        const char *params = nsr_json_obj_get(line, "params", &plen);
        if (!params)
            params = "{}";

        long long id = 0;
        bool has_id = false;
        size_t ilen;
        const char *idv = nsr_json_obj_get(line, "id", &ilen);
        if (idv) {
            has_id = true;
            nsr_json_parse_int(idv, ilen, &id);
        }

        char method_str[64];
        if (!nsr_json_parse_str(method, mlen, method_str, sizeof(method_str)))
            continue;

        if (strcmp(method_str, "init") == 0) {
            if (has_id) handle_init(params, id);
        } else if (strcmp(method_str, "update_telemetry") == 0) {
            handle_update_telemetry(params);
        } else if (strcmp(method_str, "render") == 0) {
            if (has_id) handle_render(params, id);
        } else if (strcmp(method_str, "on_key") == 0) {
            if (has_id) handle_on_key(params, id);
        } else if (strcmp(method_str, "cleanup") == 0) {
            handle_cleanup(params);
        } else if (has_id) {
            send_error(id, "Method not found");
        }
    }

    stop_sniffer();
    pthread_mutex_destroy(&g_state.lock);
    free(line);
    curl_global_cleanup();
    return 0;
}
