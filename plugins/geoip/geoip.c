#include <nsr/json/json.h>
#include <nsr/telemetry.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <curl/curl.h>

#ifdef HAVE_IP2LOCATION
#include <IP2Location.h>
#endif

#define GEO_CACHE_SIZE 256
#define GEO_COUNTRY_MAX 32
#define GEO_PROVIDER_MAX 64

typedef struct {
    char ip[48];
    char country[GEO_COUNTRY_MAX];
    char provider[GEO_PROVIDER_MAX];
    bool valid;
} geo_entry_t;

typedef struct {
#ifdef HAVE_IP2LOCATION
    IP2Location *ip2loc;
#endif
    geo_entry_t cache[GEO_CACHE_SIZE];
} geoip_state_t;

static geoip_state_t g_state;

static unsigned int geo_hash(const char *ip)
{
    unsigned int h = 5381;
    while (*ip)
        h = ((h << 5) + h) + (unsigned char)*ip++;
    return h % GEO_CACHE_SIZE;
}

static geo_entry_t *geo_cache_find(const char *ip)
{
    unsigned int idx = geo_hash(ip);
    for (int i = 0; i < GEO_CACHE_SIZE; i++) {
        geo_entry_t *e = &g_state.cache[(idx + i) % GEO_CACHE_SIZE];
        if (e->valid && strcmp(e->ip, ip) == 0)
            return e;
        if (!e->valid)
            return nullptr;
    }
    return nullptr;
}

static void geo_cache_store(const char *ip, const char *country, const char *provider)
{
    unsigned int idx = geo_hash(ip);
    for (int i = 0; i < GEO_CACHE_SIZE; i++) {
        geo_entry_t *e = &g_state.cache[(idx + i) % GEO_CACHE_SIZE];
        if (!e->valid || strcmp(e->ip, ip) == 0) {
            strncpy(e->ip, ip, sizeof(e->ip) - 1);
            e->ip[sizeof(e->ip) - 1] = '\0';
            strncpy(e->country, country ? country : "", sizeof(e->country) - 1);
            e->country[sizeof(e->country) - 1] = '\0';
            strncpy(e->provider, provider ? provider : "", sizeof(e->provider) - 1);
            e->provider[sizeof(e->provider) - 1] = '\0';
            e->valid = true;
            return;
        }
    }
}

static bool json_extract(const char *json, const char *key, char *out, size_t out_len)
{
    size_t len;
    const char *v = nsr_json_obj_get(json, key, &len);
    if (!v)
        return false;
    return nsr_json_parse_str(v, len, out, out_len);
}

struct curl_buf {
    char *buf;
    size_t cap;
    size_t len;
};

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct curl_buf *cb = (struct curl_buf *)userdata;
    size_t total = size * nmemb;
    if (cb->len + total >= cb->cap)
        total = cb->cap - cb->len - 1;
    if (total > 0) {
        memcpy(cb->buf + cb->len, ptr, total);
        cb->len += total;
        cb->buf[cb->len] = '\0';
    }
    return size * nmemb;
}

static bool web_lookup(const char *ip, char *country, size_t country_len,
                       char *provider, size_t provider_len)
{
    char url[256];
    snprintf(url, sizeof(url), "http://ip-api.com/json/%s?fields=status,country,countryCode,isp", ip);

    char buf[2048];
    CURL *curl = curl_easy_init();
    if (!curl)
        return false;

    struct curl_buf cb = { buf, sizeof(buf), 0 };
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &cb);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "nsr-geoip/0.1");
    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != 200)
        return false;
    if (strstr(buf, "\"status\":\"fail\""))
        return false;

    bool ok = false;
    char tmp[64];
    if (json_extract(buf, "countryCode", tmp, sizeof(tmp))) {
        strncpy(country, tmp, country_len - 1);
        country[country_len - 1] = '\0';
        ok = true;
    } else if (json_extract(buf, "country", tmp, sizeof(tmp))) {
        strncpy(country, tmp, country_len - 1);
        country[country_len - 1] = '\0';
        ok = true;
    }
    if (json_extract(buf, "isp", tmp, sizeof(tmp))) {
        strncpy(provider, tmp, provider_len - 1);
        provider[provider_len - 1] = '\0';
        ok = true;
    }
    return ok;
}

static bool local_lookup(const char *ip, char *country, size_t country_len,
                         char *provider, size_t provider_len)
{
#ifdef HAVE_IP2LOCATION
    if (!g_state.ip2loc)
        return false;
    IP2LocationRecord *rec = IP2Location_get_all(g_state.ip2loc, (char *)ip);
    if (!rec)
        return false;
    bool ok = false;
    if (rec->country_short && rec->country_short[0]) {
        strncpy(country, rec->country_short, country_len - 1);
        country[country_len - 1] = '\0';
        ok = true;
    }
    if (rec->isp && rec->isp[0] && strcmp(rec->isp, "-") != 0) {
        strncpy(provider, rec->isp, provider_len - 1);
        provider[provider_len - 1] = '\0';
        ok = true;
    }
    IP2Location_free_record(rec);
    return ok;
#else
    (void)ip; (void)country; (void)country_len; (void)provider; (void)provider_len;
    return false;
#endif
}

static void resolve_one(const char *ip)
{
    if (!ip || !ip[0] || strcmp(ip, "Reply Received") == 0)
        return;
    if (geo_cache_find(ip))
        return;

    char country[GEO_COUNTRY_MAX] = "";
    char provider[GEO_PROVIDER_MAX] = "";
    bool ok = local_lookup(ip, country, sizeof(country), provider, sizeof(provider));
    if (!ok)
        ok = web_lookup(ip, country, sizeof(country), provider, sizeof(provider));
    if (ok)
        geo_cache_store(ip, country, provider);
    else
        geo_cache_store(ip, "?", "?");
}

static void handle_init(const char *params, long long id)
{
    char config_path[512] = "";
    json_extract(params, "config_path", config_path, sizeof(config_path));

#ifdef HAVE_IP2LOCATION
    char path[768];
    snprintf(path, sizeof(path), "%s", config_path);
    FILE *f = fopen(path, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            nl = strchr(line, '\r');
            if (nl) *nl = '\0';
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = '\0';
            if (strcmp(line, "IP2_LOCATION_DB") == 0) {
                g_state.ip2loc = IP2Location_open(eq + 1);
                break;
            }
        }
        fclose(f);
    }
#endif

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
    nsr_json_string(&resp, "Resolve hop IPs to country & provider");
    nsr_json_obj_end(&resp);
    nsr_json_obj_end(&resp);
    printf("%s\n", nsr_json_cstr(&resp));
    fflush(stdout);
    nsr_json_free(&resp);
}

static void handle_update_telemetry(const char *params)
{
    size_t len;
    const char *hops = nsr_json_obj_get(params, "hops", &len);
    if (!hops)
        return;

    const char *next;
    const char *elem = nsr_json_arr_first(hops, &next);
    while (elem) {
        char addr[64] = "";
        json_extract(elem, "addr", addr, sizeof(addr));
        resolve_one(addr);
        elem = nsr_json_arr_next(next, &next);
    }
}

static void handle_render_hops(const char *params, long long id)
{
    size_t len;
    const char *hops = nsr_json_obj_get(params, "hops", &len);

    nsr_json_buf_t resp;
    nsr_json_init(&resp);
    nsr_json_obj_start(&resp);
    nsr_json_key(&resp, "jsonrpc");
    nsr_json_string(&resp, "2.0");
    nsr_json_key(&resp, "id");
    nsr_json_int(&resp, id);
    nsr_json_key(&resp, "result");
    nsr_json_obj_start(&resp);
    nsr_json_key(&resp, "annotations");
    nsr_json_arr_start(&resp);

    if (hops) {
        const char *next;
        const char *elem = nsr_json_arr_first(hops, &next);
        while (elem) {
            long long hop_idx = -1;
            char addr[64] = "";
            size_t flen;
            const char *v;
            v = nsr_json_obj_get(elem, "hop_idx", &flen);
            if (v) nsr_json_parse_int(v, flen, &hop_idx);
            json_extract(elem, "addr", addr, sizeof(addr));

            geo_entry_t *e = geo_cache_find(addr);
            if (e && e->valid) {
                char text[128];
                snprintf(text, sizeof(text), "[%s %s]", e->country, e->provider);
                nsr_json_obj_start(&resp);
                nsr_json_key(&resp, "hop_idx");
                nsr_json_int(&resp, hop_idx);
                nsr_json_key(&resp, "text");
                nsr_json_string(&resp, text);
                nsr_json_obj_end(&resp);
            }
            elem = nsr_json_arr_next(next, &next);
        }
    }

    nsr_json_arr_end(&resp);
    nsr_json_obj_end(&resp);
    nsr_json_obj_end(&resp);
    printf("%s\n", nsr_json_cstr(&resp));
    fflush(stdout);
    nsr_json_free(&resp);
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
    memset(&g_state, 0, sizeof(g_state));
    curl_global_init(CURL_GLOBAL_DEFAULT);

    char *line = nullptr;
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
        } else if (strcmp(method_str, "render_hops") == 0) {
            if (has_id) handle_render_hops(params, id);
        } else if (strcmp(method_str, "cleanup") == 0) {
            /* no-op */
        } else if (has_id) {
            send_error(id, "Method not found");
        }
    }

    free(line);
#ifdef HAVE_IP2LOCATION
    if (g_state.ip2loc)
        IP2Location_close(g_state.ip2loc);
#endif
    curl_global_cleanup();
    return 0;
}
