#include <nsr/json/json.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

/* ---------------- encoder ---------------- */

static void jb_grow(nsr_json_buf_t *jb, size_t need)
{
    if (jb->len + need < jb->cap)
        return;
    size_t new_cap = jb->cap ? jb->cap * 2 : 256;
    while (new_cap < jb->len + need)
        new_cap *= 2;
    jb->buf = realloc(jb->buf, new_cap);
    jb->cap = new_cap;
}

static void jb_append_raw(nsr_json_buf_t *jb, const char *s, size_t n)
{
    jb_grow(jb, n + 1);
    memcpy(jb->buf + jb->len, s, n);
    jb->len += n;
    jb->buf[jb->len] = '\0';
}

static void jb_append_char(nsr_json_buf_t *jb, char c)
{
    jb_grow(jb, 2);
    jb->buf[jb->len++] = c;
    jb->buf[jb->len] = '\0';
}

static bool needs_comma(const nsr_json_buf_t *jb)
{
    if (jb->len == 0)
        return false;
    char c = jb->buf[jb->len - 1];
    return c != '{' && c != '[' && c != ':' && c != ',';
}

void nsr_json_init(nsr_json_buf_t *jb)
{
    memset(jb, 0, sizeof(*jb));
    jb_grow(jb, 1);
    jb->buf[0] = '\0';
}

void nsr_json_reset(nsr_json_buf_t *jb)
{
    jb->len = 0;
    if (jb->buf)
        jb->buf[0] = '\0';
}

void nsr_json_free(nsr_json_buf_t *jb)
{
    free(jb->buf);
    memset(jb, 0, sizeof(*jb));
}

const char *nsr_json_cstr(const nsr_json_buf_t *jb)
{
    return jb->buf ? jb->buf : "";
}

void nsr_json_obj_start(nsr_json_buf_t *jb)
{
    if (needs_comma(jb))
        jb_append_char(jb, ',');
    jb_append_char(jb, '{');
}

void nsr_json_obj_end(nsr_json_buf_t *jb)
{
    jb_append_char(jb, '}');
}

void nsr_json_arr_start(nsr_json_buf_t *jb)
{
    if (needs_comma(jb))
        jb_append_char(jb, ',');
    jb_append_char(jb, '[');
}

void nsr_json_arr_end(nsr_json_buf_t *jb)
{
    jb_append_char(jb, ']');
}

static void append_escaped(nsr_json_buf_t *jb, const char *s)
{
    jb_append_char(jb, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  jb_append_raw(jb, "\\\"", 2); break;
        case '\\': jb_append_raw(jb, "\\\\", 2); break;
        case '\b': jb_append_raw(jb, "\\b", 2); break;
        case '\f': jb_append_raw(jb, "\\f", 2); break;
        case '\n': jb_append_raw(jb, "\\n", 2); break;
        case '\r': jb_append_raw(jb, "\\r", 2); break;
        case '\t': jb_append_raw(jb, "\\t", 2); break;
        default:
            if (*p < 0x20) {
                char esc[7];
                snprintf(esc, sizeof(esc), "\\u%04x", *p);
                jb_append_raw(jb, esc, strlen(esc));
            } else {
                jb_append_char(jb, (char)*p);
            }
        }
    }
    jb_append_char(jb, '"');
}

void nsr_json_key(nsr_json_buf_t *jb, const char *key)
{
    if (needs_comma(jb))
        jb_append_char(jb, ',');
    append_escaped(jb, key);
    jb_append_char(jb, ':');
}

void nsr_json_string(nsr_json_buf_t *jb, const char *val)
{
    if (needs_comma(jb))
        jb_append_char(jb, ',');
    append_escaped(jb, val ? val : "");
}

void nsr_json_int(nsr_json_buf_t *jb, long long val)
{
    if (needs_comma(jb))
        jb_append_char(jb, ',');
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%lld", val);
    jb_append_raw(jb, tmp, strlen(tmp));
}

void nsr_json_double(nsr_json_buf_t *jb, double val)
{
    if (needs_comma(jb))
        jb_append_char(jb, ',');
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%.6f", val);
    jb_append_raw(jb, tmp, strlen(tmp));
}

void nsr_json_bool(nsr_json_buf_t *jb, bool val)
{
    if (needs_comma(jb))
        jb_append_char(jb, ',');
    jb_append_raw(jb, val ? "true" : "false", val ? 4 : 5);
}

void nsr_json_null(nsr_json_buf_t *jb)
{
    if (needs_comma(jb))
        jb_append_char(jb, ',');
    jb_append_raw(jb, "null", 4);
}

void nsr_json_append_raw(nsr_json_buf_t *jb, const char *s, size_t n)
{
    jb_append_raw(jb, s, n);
}

/* ---------------- decoder ---------------- */

static const char *skip_ws(const char *p)
{
    while (*p && isspace((unsigned char)*p))
        p++;
    return p;
}

static const char *skip_str(const char *p)
{
    if (*p != '"')
        return NULL;
    p++;
    while (*p) {
        if (*p == '\\' && p[1]) {
            p += 2;
        } else if (*p == '"') {
            p++;
            return p;
        } else {
            p++;
        }
    }
    return NULL;
}

static const char *skip_value(const char *p)
{
    p = skip_ws(p);
    if (*p == '"') {
        return skip_str(p);
    } else if (*p == '{') {
        p++;
        while (1) {
            p = skip_ws(p);
            if (*p == '}')
                return p + 1;
            if (*p == '"') {
                p = skip_str(p);
                if (!p) return NULL;
                p = skip_ws(p);
                if (*p == ':') p++;
                p = skip_value(p);
                if (!p) return NULL;
                p = skip_ws(p);
                if (*p == ',') { p++; continue; }
                if (*p == '}') return p + 1;
                return NULL;
            }
            return NULL;
        }
    } else if (*p == '[') {
        p++;
        while (1) {
            p = skip_ws(p);
            if (*p == ']')
                return p + 1;
            p = skip_value(p);
            if (!p) return NULL;
            p = skip_ws(p);
            if (*p == ',') { p++; continue; }
            if (*p == ']') return p + 1;
            return NULL;
        }
    } else {
        while (*p && *p != ',' && *p != '}' && *p != ']')
            p++;
        return p;
    }
}

const char *nsr_json_obj_get(const char *json, const char *key, size_t *out_len)
{
    const char *p = skip_ws(json);
    if (*p != '{')
        return NULL;
    p++;
    while (1) {
        p = skip_ws(p);
        if (*p == '}')
            return NULL;
        if (*p != '"')
            return NULL;
        const char *key_start = p + 1;
        const char *key_end = skip_str(p);
        if (!key_end)
            return NULL;
        size_t key_len = (size_t)(key_end - key_start - 1);
        bool match = (strlen(key) == key_len && strncmp(key_start, key, key_len) == 0);

        p = skip_ws(key_end);
        if (*p != ':')
            return NULL;
        p++;
        p = skip_ws(p);
        const char *val_start = p;
        p = skip_value(p);
        if (!p)
            return NULL;
        const char *val_end = p;

        if (match) {
            *out_len = (size_t)(val_end - val_start);
            return val_start;
        }

        p = skip_ws(p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == '}')
            return NULL;
        return NULL;
    }
}

bool nsr_json_parse_str(const char *val, size_t val_len, char *out, size_t out_len)
{
    if (val_len < 2 || val[0] != '"' || val[val_len - 1] != '"')
        return false;
    const char *p = val + 1;
    const char *end = val + val_len - 1;
    size_t o = 0;
    while (p < end) {
        if (*p == '\\' && p + 1 < end) {
            switch (p[1]) {
            case '"':  if (o < out_len - 1) out[o++] = '"';  break;
            case '\\': if (o < out_len - 1) out[o++] = '\\'; break;
            case '/':  if (o < out_len - 1) out[o++] = '/';  break;
            case 'b':  if (o < out_len - 1) out[o++] = '\b'; break;
            case 'f':  if (o < out_len - 1) out[o++] = '\f'; break;
            case 'n':  if (o < out_len - 1) out[o++] = '\n'; break;
            case 'r':  if (o < out_len - 1) out[o++] = '\r'; break;
            case 't':  if (o < out_len - 1) out[o++] = '\t'; break;
            default:   if (o < out_len - 1) out[o++] = p[1]; break;
            }
            p += 2;
        } else {
            if (o < out_len - 1)
                out[o++] = *p;
            p++;
        }
    }
    if (o < out_len)
        out[o] = '\0';
    else if (out_len > 0)
        out[out_len - 1] = '\0';
    return true;
}

bool nsr_json_parse_int(const char *val, size_t val_len, long long *out)
{
    char tmp[32];
    if (val_len >= sizeof(tmp))
        return false;
    memcpy(tmp, val, val_len);
    tmp[val_len] = '\0';
    char *end;
    *out = strtoll(tmp, &end, 10);
    return end == tmp + val_len;
}

bool nsr_json_parse_double(const char *val, size_t val_len, double *out)
{
    char tmp[64];
    if (val_len >= sizeof(tmp))
        return false;
    memcpy(tmp, val, val_len);
    tmp[val_len] = '\0';
    char *end;
    *out = strtod(tmp, &end);
    return end == tmp + val_len;
}

bool nsr_json_parse_bool(const char *val, size_t val_len, bool *out)
{
    if (val_len == 4 && memcmp(val, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (val_len == 5 && memcmp(val, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

const char *nsr_json_arr_first(const char *json, const char **next)
{
    const char *p = skip_ws(json);
    if (*p != '[')
        return NULL;
    p++;
    p = skip_ws(p);
    if (*p == ']') {
        *next = p + 1;
        return NULL;
    }
    const char *elem = p;
    p = skip_value(p);
    if (!p)
        return NULL;
    *next = p;
    return elem;
}

const char *nsr_json_arr_next(const char *next, const char **new_next)
{
    const char *p = skip_ws(next);
    if (*p == ',') {
        p++;
        p = skip_ws(p);
        const char *elem = p;
        p = skip_value(p);
        if (!p)
            return NULL;
        *new_next = p;
        return elem;
    }
    if (*p == ']') {
        *new_next = p + 1;
        return NULL;
    }
    return NULL;
}
