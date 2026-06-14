#include <nsr/plugin/plugin.h>
#include <nsr/ui/tui.h>
#include <nsr/json/json.h>
#include <ncursesw/curses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void config_set_bool(const char *path, const char *key, bool value)
{
    if (!path || !key)
        return;

    FILE *in = fopen(path, "r");
    char *lines[256] = {0};
    int n_lines = 0;
    bool found = false;

    if (in) {
        char line[512];
        while (fgets(line, sizeof(line), in) && n_lines < 256) {
            char tmp[512];
            strncpy(tmp, line, sizeof(tmp) - 1);
            tmp[sizeof(tmp) - 1] = '\0';
            char *nl = strchr(tmp, '\n');
            if (nl) *nl = '\0';
            nl = strchr(tmp, '\r');
            if (nl) *nl = '\0';

            char *eq = strchr(tmp, '=');
            if (eq) {
                *eq = '\0';
                if (strcmp(tmp, key) == 0) {
                    char buf[512];
                    snprintf(buf, sizeof(buf), "%s=%d\n", key, value ? 1 : 0);
                    lines[n_lines++] = strdup(buf);
                    found = true;
                    continue;
                }
            }
            lines[n_lines++] = strdup(line);
        }
        fclose(in);
    }

    if (!found) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s=%d\n", key, value ? 1 : 0);
        if (n_lines < 256)
            lines[n_lines++] = strdup(buf);
    }

    FILE *out = fopen(path, "w");
    if (out) {
        for (int i = 0; i < n_lines; i++) {
            fputs(lines[i], out);
            free(lines[i]);
        }
        fclose(out);
    } else {
        for (int i = 0; i < n_lines; i++)
            free(lines[i]);
    }
}

static bool config_get_bool_default(const char *path, const char *key, bool def)
{
    if (!path || !key)
        return def;
    FILE *f = fopen(path, "r");
    if (!f)
        return def;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        nl = strchr(line, '\r');
        if (nl) *nl = '\0';

        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        if (strcmp(line, key) == 0) {
            fclose(f);
            return atoi(eq + 1) != 0;
        }
    }
    fclose(f);
    return def;
}

static bool is_executable(const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0)
        return false;
    if (!S_ISREG(st.st_mode))
        return false;
    return (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
}

void nsr_plugins_init(nsr_plugin_registry_t *reg, const char *config_path)
{
    memset(reg, 0, sizeof(*reg));
    if (config_path)
        strncpy(reg->config_path, config_path, sizeof(reg->config_path) - 1);
}

static void plugin_stop(nsr_plugin_entry_t *e)
{
    if (e->rpc.pid > 0) {
        nsr_json_rpc_notify(&e->rpc, "cleanup", NULL);
        nsr_json_rpc_close(&e->rpc);
    }
    nsr_json_free(&e->last_render_resp);
    nsr_json_free(&e->last_render_hops_resp);
    e->reserved_count = 0;
    e->initialized = false;
    e->dead = false;
    e->render_pending = false;
    e->render_hops_pending = false;
    e->on_key_pending = false;
    e->is_modal = false;
    e->pending_render_id = -1;
    e->pending_render_hops_id = -1;
    e->pending_on_key_id = -1;
}

void nsr_plugins_cleanup(nsr_plugin_registry_t *reg)
{
    if (!reg)
        return;
    for (int i = 0; i < reg->count; i++)
        plugin_stop(&reg->entries[i]);
    memset(reg, 0, sizeof(*reg));
}

static int char_cmp(const void *a, const void *b)
{
    return *(const char *)a - *(const char *)b;
}

static void parse_reserved_keys(nsr_plugin_entry_t *e, const char *val, size_t val_len)
{
    e->reserved_count = 0;
    if (!val || val_len == 0)
        return;

    char tmp[NSR_KEY_USER_LEN];
    int n = 0;
    for (size_t i = 0; i < val_len && n < NSR_KEY_USER_LEN; i++) {
        char c = val[i];
        if (c == '\"' || c == '\'' || c == ',' || c == ' ' || c == '\t')
            continue;
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            tmp[n++] = c;
    }

    qsort(tmp, (size_t)n, sizeof(char), char_cmp);
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (i == 0 || tmp[i] != tmp[i - 1])
            e->reserved_keys[m++] = tmp[i];
    }
    e->reserved_count = m;
}

static bool plugin_spawn(nsr_plugin_entry_t *e, const char *config_path,
                         const char *plugin_dir)
{
    if (!nsr_json_rpc_spawn(&e->rpc, e->path))
        return false;

    nsr_json_buf_t params;
    nsr_json_init(&params);
    nsr_json_obj_start(&params);
    nsr_json_key(&params, "config_path");
    nsr_json_string(&params, config_path);
    nsr_json_key(&params, "plugin_dir");
    nsr_json_string(&params, plugin_dir);
    nsr_json_obj_end(&params);

    nsr_json_buf_t resp;
    nsr_json_init(&resp);
    bool ok = nsr_json_rpc_call(&e->rpc, "init", &params, 2000, &resp);
    nsr_json_free(&params);

    if (ok) {
        size_t len;
        const char *result = nsr_json_obj_get(nsr_json_cstr(&resp), "result", &len);
        if (result) {
            const char *status = nsr_json_obj_get(result, "status", &len);
            if (status && len == 4 && strncmp(status, "\"ok\"", 4) == 0) {
                const char *desc = nsr_json_obj_get(result, "description", &len);
                if (desc) {
                    nsr_json_parse_str(desc, len, e->description, sizeof(e->description));
                }
                const char *rks = nsr_json_obj_get(result, "reserved_keys", &len);
                if (rks)
                    parse_reserved_keys(e, rks, len);
                e->initialized = true;
                nsr_json_free(&resp);
                return true;
            }
        }
    }

    nsr_json_free(&resp);
    nsr_json_rpc_close(&e->rpc);
    e->dead = true;
    return false;
}

int nsr_plugins_load_dir(nsr_plugin_registry_t *reg, const char *dir)
{
    if (!reg || !dir)
        return -1;
    strncpy(reg->plugin_dir, dir, sizeof(reg->plugin_dir) - 1);

    DIR *d = opendir(dir);
    if (!d)
        return -1;

    int loaded = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;

        char path[768];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        if (!is_executable(path))
            continue;

        bool dup = false;
        for (int i = 0; i < reg->count; i++) {
            if (strcmp(reg->entries[i].name, ent->d_name) == 0) {
                dup = true;
                break;
            }
        }
        if (dup)
            continue;
        if (reg->count >= NSR_MAX_PLUGINS)
            break;

        nsr_plugin_entry_t *e = &reg->entries[reg->count++];
        memset(e, 0, sizeof(*e));
        strncpy(e->name, ent->d_name, sizeof(e->name) - 1);
        strncpy(e->path, path, sizeof(e->path) - 1);

        char key[128];
        snprintf(key, sizeof(key), "NSR_PLUGIN_%s", e->name);
        e->enabled = config_get_bool_default(reg->config_path, key, false);

        if (e->enabled)
            plugin_spawn(e, reg->config_path, reg->plugin_dir);
        loaded++;
    }
    closedir(d);
    return loaded;
}

static void plugin_check_alive(nsr_plugin_entry_t *e)
{
    if (!e->initialized || e->rpc.pid <= 0)
        return;
    int status;
    if (waitpid(e->rpc.pid, &status, WNOHANG) != 0) {
        e->dead = true;
        e->initialized = false;
    }
}

void nsr_plugins_update_telemetry(nsr_plugin_registry_t *reg,
                                  const nsr_telemetry_state_t *tel,
                                  const nsr_topology_state_t *topo)
{
    (void)topo;
    if (!reg)
        return;

    nsr_json_buf_t params;
    nsr_json_init(&params);
    nsr_json_obj_start(&params);
    nsr_json_key(&params, "target_ip");
    nsr_json_string(&params, tel->target_ip);
    nsr_json_key(&params, "target_host");
    nsr_json_string(&params, tel->target_host);
    nsr_json_key(&params, "interval_ms");
    nsr_json_int(&params, tel->interval_ms);
    nsr_json_key(&params, "hops");
    nsr_json_arr_start(&params);
    for (int i = 1; i < NSR_MAX_HOPS; i++) {
        const nsr_hop_info_t *h = &tel->hops[i];
        if (h->sent == 0)
            continue;
        nsr_json_obj_start(&params);
        nsr_json_key(&params, "hop_idx");
        nsr_json_int(&params, i);
        nsr_json_key(&params, "addr");
        nsr_json_string(&params, h->addr);
        nsr_json_key(&params, "rtt_us");
        nsr_json_int(&params, (long long)h->rtt_us);
        nsr_json_key(&params, "sent");
        nsr_json_int(&params, h->sent);
        nsr_json_key(&params, "recv");
        nsr_json_int(&params, h->recv);
        nsr_json_key(&params, "loss");
        nsr_json_double(&params, h->sent ? 1.0 - (double)h->recv / h->sent : 0.0);
        nsr_json_key(&params, "status");
        switch (h->last_status) {
        case NSR_OBS_REPLY:    nsr_json_string(&params, "reply"); break;
        case NSR_OBS_EXCEEDED: nsr_json_string(&params, "exceeded"); break;
        case NSR_OBS_UNREACH:  nsr_json_string(&params, "unreachable"); break;
        case NSR_OBS_TIMEOUT:  nsr_json_string(&params, "timeout"); break;
        default:               nsr_json_string(&params, "probing"); break;
        }
        nsr_json_obj_end(&params);
    }
    nsr_json_arr_end(&params);
    nsr_json_obj_end(&params);

    for (int i = 0; i < reg->count; i++) {
        nsr_plugin_entry_t *e = &reg->entries[i];
        plugin_check_alive(e);
        if (e->enabled && e->initialized && !e->dead)
            nsr_json_rpc_notify(&e->rpc, "update_telemetry", &params);
    }
    nsr_json_free(&params);
}

static int color_for_name(const char *name)
{
    if (strcmp(name, "cyan") == 0)    return COLOR_PAIR(1);
    if (strcmp(name, "green") == 0)   return COLOR_PAIR(2);
    if (strcmp(name, "yellow") == 0)  return COLOR_PAIR(3);
    if (strcmp(name, "red") == 0)     return COLOR_PAIR(4);
    if (strcmp(name, "magenta") == 0) return COLOR_PAIR(5);
    if (strcmp(name, "blue") == 0)    return COLOR_PAIR(6);
    if (strcmp(name, "white") == 0)   return COLOR_PAIR(7);
    if (strcmp(name, "highlight") == 0) return COLOR_PAIR(8);
    return 0;
}

static void draw_render_response(const char *json, int y, int x)
{
    size_t len;
    const char *result = nsr_json_obj_get(json, "result", &len);
    if (!result)
        return;
    const char *lines = nsr_json_obj_get(result, "lines", &len);
    if (!lines)
        return;

    const char *next;
    const char *elem = nsr_json_arr_first(lines, &next);
    while (elem) {
        long long ly = -1, lx = -1;
        char text[128] = "";
        char color[32] = "";
        size_t flen;
        const char *v;
        v = nsr_json_obj_get(elem, "y", &flen);
        if (v) nsr_json_parse_int(v, flen, &ly);
        v = nsr_json_obj_get(elem, "x", &flen);
        if (v) nsr_json_parse_int(v, flen, &lx);
        v = nsr_json_obj_get(elem, "text", &flen);
        if (v) nsr_json_parse_str(v, flen, text, sizeof(text));
        v = nsr_json_obj_get(elem, "color", &flen);
        if (v) nsr_json_parse_str(v, flen, color, sizeof(color));

        if (ly >= 0 && lx >= 0 && text[0]) {
            int pair = color_for_name(color);
            if (pair) attron(pair);
            mvprintw((int)ly + y, (int)lx + x, "%s", text);
            if (pair) attroff(pair);
        }
        elem = nsr_json_arr_next(next, &next);
    }
}

static bool plugin_drain_response(nsr_plugin_entry_t *e, int timeout_ms)
{
    if (e->rpc.dead || e->rpc.out < 0)
        return false;
    if (!e->render_pending && !e->render_hops_pending && !e->on_key_pending)
        return false;

    nsr_json_buf_t resp;
    nsr_json_init(&resp);
    long long id = -1;
    if (!nsr_json_rpc_try_recv_any(&e->rpc, timeout_ms, &resp, &id)) {
        nsr_json_free(&resp);
        return false;
    }

    if (e->render_pending && id == e->pending_render_id) {
        nsr_json_free(&e->last_render_resp);
        e->last_render_resp = resp;
        e->render_pending = false;
        return true;
    }
    if (e->render_hops_pending && id == e->pending_render_hops_id) {
        nsr_json_free(&e->last_render_hops_resp);
        e->last_render_hops_resp = resp;
        e->render_hops_pending = false;
        return true;
    }
    if (e->on_key_pending && id == e->pending_on_key_id) {
        nsr_json_free(&resp);
        e->on_key_pending = false;
        return true;
    }
    nsr_json_free(&resp);
    return true;
}

void nsr_plugins_render(nsr_plugin_registry_t *reg,
                        const nsr_tui_state_t *tui,
                        const nsr_topology_state_t *topo,
                        int y, int x, int h, int w,
                        int screen_h, int screen_w)
{
    const int SNIFFER_PANEL_H = 18;
    const int SNIFFER_PANEL_W = 60;
    if (!reg || !tui)
        return;

    for (int i = 0; i < reg->count; i++) {
        nsr_plugin_entry_t *e = &reg->entries[i];
        plugin_check_alive(e);
        if (!e->enabled || !e->initialized || e->dead)
            continue;

        e->is_modal = false;
        long long m_w = 60, m_h = 13;
        if (e->last_render_resp.len > 0) {
            size_t mlen;
            const char *res = nsr_json_obj_get(nsr_json_cstr(&e->last_render_resp), "result", &mlen);
            if (res) {
                const char *mv = nsr_json_obj_get(res, "is_modal", &mlen);
                if (mv) {
                    nsr_json_parse_bool(mv, mlen, &e->is_modal);
                }
                const char *wv = nsr_json_obj_get(res, "modal_width", &mlen);
                if (wv) nsr_json_parse_int(wv, mlen, &m_w);
                const char *hv = nsr_json_obj_get(res, "modal_height", &mlen);
                if (hv) nsr_json_parse_int(hv, mlen, &m_h);
            }
        }

        int py = y, px = x, ph = h, pw = w;
        if (e->is_modal) {
            ph = (int)m_h;
            pw = (int)m_w;
            py = (screen_h - ph) / 2;
            px = (screen_w - pw) / 2;
            if (py < 0) py = 0;
            if (px < 0) px = 0;

            for (int r = 0; r < ph; r++) {
                mvhline(py + r, px, ' ', pw);
            }

            attron(COLOR_PAIR(3)); 
            mvhline(py, px, 0, pw);
            mvhline(py + ph - 1, px, 0, pw);
            mvvline(py, px, 0, ph);
            mvvline(py, px + pw - 1, 0, ph);
            mvaddch(py, px, ACS_ULCORNER);
            mvaddch(py, px + pw - 1, ACS_URCORNER);
            mvaddch(py + ph - 1, px, ACS_LLCORNER);
            mvaddch(py + ph - 1, px + pw - 1, ACS_LRCORNER);
            attroff(COLOR_PAIR(3));
        } else if (strcmp(e->name, "sniffer") == 0) {
            ph = SNIFFER_PANEL_H;
            pw = SNIFFER_PANEL_W;
            py = screen_h - ph - 2;
            px = screen_w - pw - 2;
            if (py < 0) py = 0;
            if (px < 0) px = 0;

            for (int r = 0; r < ph; r++) {
                mvhline(py + r, px, ' ', pw);
            }

            attron(COLOR_PAIR(1)); 
            mvhline(py, px, 0, pw);
            mvhline(py + ph - 1, px, 0, pw);
            mvvline(py, px, 0, ph);
            mvvline(py, px + pw - 1, 0, ph);
            mvaddch(py, px, ACS_ULCORNER);
            mvaddch(py, px + pw - 1, ACS_URCORNER);
            mvaddch(py + ph - 1, px, ACS_LLCORNER);
            mvaddch(py + ph - 1, px + pw - 1, ACS_LRCORNER);
            attroff(COLOR_PAIR(1));
        }

        if (e->last_render_resp.len > 0)
            draw_render_response(nsr_json_cstr(&e->last_render_resp), py, px);

        plugin_drain_response(e, 5);

        if (!e->render_pending) {
            nsr_json_buf_t params;
            nsr_json_init(&params);
            nsr_json_obj_start(&params);
            nsr_json_key(&params, "mode");
            switch (tui->current_mode) {
            case NSR_UI_GRID:  nsr_json_string(&params, "grid"); break;
            case NSR_UI_TREE:  nsr_json_string(&params, "tree"); break;
            case NSR_UI_TOOLS: nsr_json_string(&params, "tools"); break;
            default:           nsr_json_string(&params, "normal"); break;
            }
            nsr_json_key(&params, "width");
            nsr_json_int(&params, pw);
            nsr_json_key(&params, "height");
            nsr_json_int(&params, ph);
            nsr_json_key(&params, "focused_node_id");
            nsr_json_int(&params, (long long)tui->focused_node_id);
            nsr_json_key(&params, "focused_addr");
            if (topo && tui->focused_node_id) {
                const char *addr = NULL;
                for (int i = 0; i < NSR_TOPOLOGY_MAX_NODES; i++) {
                    if (topo->nodes[i].active && topo->nodes[i].id == tui->focused_node_id) {
                        addr = topo->nodes[i].addr;
                        break;
                    }
                }
                nsr_json_string(&params, addr ? addr : "");
            } else {
                nsr_json_string(&params, "");
            }
            nsr_json_obj_end(&params);

            if (nsr_json_rpc_send_request(&e->rpc, "render", &params,
                                          &e->pending_render_id)) {
                e->render_pending = true;
            } else if (e->rpc.dead)
                e->dead = true;

            nsr_json_free(&params);
        }
    }
}

static int parse_render_hops_response(const char *json,
                                      nsr_hop_annotation_t *out,
                                      int max_out,
                                      int total)
{
    size_t len;
    const char *result = nsr_json_obj_get(json, "result", &len);
    if (!result)
        return total;
    const char *annotations = nsr_json_obj_get(result, "annotations", &len);
    if (!annotations)
        return total;

    const char *next;
    const char *elem = nsr_json_arr_first(annotations, &next);
    while (elem && total < max_out) {
        long long hop_idx = -1;
        char text[128] = "";
        size_t flen;
        const char *v;
        v = nsr_json_obj_get(elem, "hop_idx", &flen);
        if (v) nsr_json_parse_int(v, flen, &hop_idx);
        v = nsr_json_obj_get(elem, "text", &flen);
        if (v) nsr_json_parse_str(v, flen, text, sizeof(text));
        if (hop_idx >= 0 && text[0]) {
            out[total].hop_idx = (int)hop_idx;
            strncpy(out[total].text, text, sizeof(out[total].text) - 1);
            out[total].text[sizeof(out[total].text) - 1] = '\0';
            total++;
        }
        elem = nsr_json_arr_next(next, &next);
    }
    return total;
}

int nsr_plugins_render_hops(nsr_plugin_registry_t *reg,
                            const nsr_hop_info_t *hops,
                            int hop_count,
                            nsr_hop_annotation_t *out,
                            int max_out)
{
    if (!reg || !hops || !out || max_out <= 0)
        return 0;

    nsr_json_buf_t params;
    nsr_json_init(&params);
    nsr_json_obj_start(&params);
    nsr_json_key(&params, "hops");
    nsr_json_arr_start(&params);
    for (int i = 1; i < NSR_MAX_HOPS && i < hop_count; i++) {
        const nsr_hop_info_t *h = &hops[i];
        if (h->sent == 0)
            continue;
        nsr_json_obj_start(&params);
        nsr_json_key(&params, "hop_idx");
        nsr_json_int(&params, i);
        nsr_json_key(&params, "addr");
        nsr_json_string(&params, h->addr);
        nsr_json_key(&params, "rtt_us");
        nsr_json_int(&params, (long long)h->rtt_us);
        nsr_json_key(&params, "sent");
        nsr_json_int(&params, h->sent);
        nsr_json_key(&params, "recv");
        nsr_json_int(&params, h->recv);
        nsr_json_key(&params, "loss");
        nsr_json_double(&params, h->sent ? 1.0 - (double)h->recv / h->sent : 0.0);
        nsr_json_key(&params, "status");
        switch (h->last_status) {
        case NSR_OBS_REPLY:    nsr_json_string(&params, "reply"); break;
        case NSR_OBS_EXCEEDED: nsr_json_string(&params, "exceeded"); break;
        case NSR_OBS_UNREACH:  nsr_json_string(&params, "unreachable"); break;
        case NSR_OBS_TIMEOUT:  nsr_json_string(&params, "timeout"); break;
        default:               nsr_json_string(&params, "probing"); break;
        }
        nsr_json_obj_end(&params);
    }
    nsr_json_arr_end(&params);
    nsr_json_obj_end(&params);

    int total = 0;
    for (int i = 0; i < reg->count && total < max_out; i++) {
        nsr_plugin_entry_t *e = &reg->entries[i];
        plugin_check_alive(e);
        if (!e->enabled || !e->initialized || e->dead)
            continue;

        if (e->last_render_hops_resp.len > 0)
            total = parse_render_hops_response(nsr_json_cstr(&e->last_render_hops_resp),
                                               out, max_out, total);

        plugin_drain_response(e, 5);

        if (!e->render_hops_pending) {
            if (nsr_json_rpc_send_request(&e->rpc, "render_hops", &params,
                                          &e->pending_render_hops_id)) {
                e->render_hops_pending = true;
            } else if (e->rpc.dead)
                e->dead = true;
        }
    }
    nsr_json_free(&params);
    return total;
}

static bool plugin_reserves_key(const nsr_plugin_entry_t *e, int ch)
{
    if (ch < 0 || ch > 127)
        return false;
    char key = (char)ch;
    if (key >= 'A' && key <= 'Z')
        key += 'a' - 'A';
    for (int i = 0; i < e->reserved_count; i++) {
        if (e->reserved_keys[i] == key)
            return true;
    }
    return false;
}

static bool plugin_try_on_key(nsr_plugin_entry_t *e,
                              const nsr_json_buf_t *params)
{
    while (plugin_drain_response(e, 0))
        ;

    nsr_json_buf_t resp;
    nsr_json_init(&resp);
    if (!nsr_json_rpc_send_request(&e->rpc, "on_key", params,
                                   &e->pending_on_key_id)) {
        nsr_json_free(&resp);
        if (e->rpc.dead)
            e->dead = true;
        return false;
    }
    e->on_key_pending = true;

    long long id = -1;
    bool got = nsr_json_rpc_try_recv_any(&e->rpc, 10, &resp, &id);
    if (got && id == e->pending_on_key_id) {
        e->on_key_pending = false;
        size_t len;
        const char *result = nsr_json_obj_get(nsr_json_cstr(&resp), "result", &len);
        if (result) {
            
            // 핵심 수정: IPC 응답이 떨어지자마자 호스트 단의 is_modal 상태 즉시 갱신
            const char *mv = nsr_json_obj_get(result, "is_modal", &len);
            if (mv) {
                nsr_json_parse_bool(mv, len, &e->is_modal);
            }

            const char *act_val = nsr_json_obj_get(result, "action", &len);
            char action_buf[64] = "";
            if (act_val) {
                nsr_json_parse_str(act_val, len, action_buf, sizeof(action_buf));
            }
            if (strcmp(action_buf, "open_editor") == 0) {
                char file_buf[256] = "";
                const char *file_val = nsr_json_obj_get(result, "file", &len);
                if (file_val) {
                    nsr_json_parse_str(file_val, len, file_buf, sizeof(file_buf));
                }
                if (file_buf[0]) {
                    def_prog_mode();
                    endwin();

                    char cmd[512];
                    const char *editor = getenv("EDITOR");
                    if (!editor || !editor[0]) {
                        editor = "nano";
                    }
                    snprintf(cmd, sizeof(cmd), "%s %s", editor, file_buf);
                    int ret = system(cmd);
                    (void)ret;

                    reset_prog_mode();
                    refresh();
                }
                nsr_json_free(&resp);
                return true;
            }

            const char *hv = nsr_json_obj_get(result, "handled", &len);
            bool h = false;
            if (hv && nsr_json_parse_bool(hv, len, &h) && h) {
                nsr_json_free(&resp);
                nsr_json_free(&e->last_render_resp);
                nsr_json_init(&e->last_render_resp);
                return true;
            }
        }
        nsr_json_free(&resp);
    } else {
        nsr_json_free(&resp);
        if (e->rpc.dead) {
            e->dead = true;
            e->on_key_pending = false;
        }
    }
    return false;
}

bool nsr_plugins_on_key(nsr_plugin_registry_t *reg, int ch)
{
    if (!reg || ch < 0 || ch > 127)
        return false;

    nsr_json_buf_t params;
    nsr_json_init(&params);
    nsr_json_obj_start(&params);
    nsr_json_key(&params, "key");
    nsr_json_int(&params, ch);
    nsr_json_obj_end(&params);

    bool handled = false;
    bool targeted = false;

    /* Zero pass: if a plugin is currently modal, it gets priority for ALL keys. */
    for (int i = 0; i < reg->count && !handled; i++) {
        nsr_plugin_entry_t *e = &reg->entries[i];
        plugin_check_alive(e);
        if (!e->enabled || !e->initialized || e->dead)
            continue;
        
        // 핵심 수정: 렌더 캐시 의존 제거, 최신화된 호스트의 is_modal 상태 직접 활용
        if (e->is_modal) {
            if (plugin_try_on_key(e, &params)) {
                handled = true;
            }
        }
    }

    /* First pass: plugins that explicitly reserved this key. */
    for (int i = 0; i < reg->count && !handled; i++) {
        nsr_plugin_entry_t *e = &reg->entries[i];
        plugin_check_alive(e);
        if (!e->enabled || !e->initialized || e->dead)
            continue;
        if (!plugin_reserves_key(e, ch))
            continue;
        targeted = true;
        if (plugin_try_on_key(e, &params))
            handled = true;
    }

    /* Second pass: generic on_key broadcast for unreserved keys. */
    if (!targeted) {
        for (int i = 0; i < reg->count && !handled; i++) {
            nsr_plugin_entry_t *e = &reg->entries[i];
            plugin_check_alive(e);
            if (!e->enabled || !e->initialized || e->dead)
                continue;
            if (plugin_try_on_key(e, &params))
                handled = true;
        }
    }

    nsr_json_free(&params);
    return handled;
}

int nsr_plugins_count(const nsr_plugin_registry_t *reg)
{
    return reg ? reg->count : 0;
}

const char *nsr_plugins_name(const nsr_plugin_registry_t *reg, int idx)
{
    if (!reg || idx < 0 || idx >= reg->count)
        return NULL;
    return reg->entries[idx].name;
}

const char *nsr_plugins_description(const nsr_plugin_registry_t *reg, int idx)
{
    if (!reg || idx < 0 || idx >= reg->count)
        return NULL;
    return reg->entries[idx].description[0] ? reg->entries[idx].description : reg->entries[idx].name;
}

bool nsr_plugins_enabled(const nsr_plugin_registry_t *reg, int idx)
{
    if (!reg || idx < 0 || idx >= reg->count)
        return false;
    return reg->entries[idx].enabled;
}

void nsr_plugins_set_enabled(nsr_plugin_registry_t *reg, int idx, bool enabled)
{
    if (!reg || idx < 0 || idx >= reg->count)
        return;
    nsr_plugin_entry_t *e = &reg->entries[idx];
    if (e->enabled == enabled)
        return;

    e->enabled = enabled;
    char key[128];
    snprintf(key, sizeof(key), "NSR_PLUGIN_%s", e->name);
    config_set_bool(reg->config_path, key, enabled);

    if (enabled) {
        plugin_spawn(e, reg->config_path, reg->plugin_dir);
    } else {
        plugin_stop(e);
    }
}

/* ============================================================
 * Pseudo-OOP manager implementation.
 * ============================================================ */

static void plugin_mgr_init(nsr_plugin_manager_t *self, const char *config_path)
{
    nsr_plugins_init(&self->registry, config_path);
}

static void plugin_mgr_cleanup(nsr_plugin_manager_t *self)
{
    nsr_plugins_cleanup(&self->registry);
}

static int plugin_mgr_load_dir(nsr_plugin_manager_t *self, const char *dir)
{
    return nsr_plugins_load_dir(&self->registry, dir);
}

static void plugin_mgr_update_telemetry(nsr_plugin_manager_t *self,
                                        const nsr_telemetry_state_t *tel,
                                        const nsr_topology_state_t *topo)
{
    nsr_plugins_update_telemetry(&self->registry, tel, topo);
}

static void plugin_mgr_render(nsr_plugin_manager_t *self,
                              const struct nsr_tui_state *tui,
                              const nsr_topology_state_t *topo,
                              int y, int x, int h, int w,
                              int screen_h, int screen_w)
{
    nsr_plugins_render(&self->registry, tui, topo, y, x, h, w, screen_h, screen_w);
}

static int plugin_mgr_render_hops(nsr_plugin_manager_t *self,
                                  const nsr_hop_info_t *hops,
                                  int hop_count,
                                  nsr_hop_annotation_t *out,
                                  int max_out)
{
    return nsr_plugins_render_hops(&self->registry, hops, hop_count, out, max_out);
}

static bool plugin_mgr_on_key(nsr_plugin_manager_t *self, int ch)
{
    return nsr_plugins_on_key(&self->registry, ch);
}

static int plugin_mgr_count(const nsr_plugin_manager_t *self)
{
    return nsr_plugins_count(&self->registry);
}

static const char *plugin_mgr_name(const nsr_plugin_manager_t *self, int idx)
{
    return nsr_plugins_name(&self->registry, idx);
}

static const char *plugin_mgr_description(const nsr_plugin_manager_t *self, int idx)
{
    return nsr_plugins_description(&self->registry, idx);
}

bool plugin_mgr_enabled(const nsr_plugin_manager_t *self, int idx)
{
    return nsr_plugins_enabled(&self->registry, idx);
}

static void plugin_mgr_set_enabled(nsr_plugin_manager_t *self, int idx, bool enabled)
{
    nsr_plugins_set_enabled(&self->registry, idx, enabled);
}

const struct nsr_plugin_manager_vtable nsr_plugin_manager_vtable = {
    .init = plugin_mgr_init,
    .cleanup = plugin_mgr_cleanup,
    .load_dir = plugin_mgr_load_dir,
    .update_telemetry = plugin_mgr_update_telemetry,
    .render = plugin_mgr_render,
    .render_hops = plugin_mgr_render_hops,
    .on_key = plugin_mgr_on_key,
    .count = plugin_mgr_count,
    .name = plugin_mgr_name,
    .description = plugin_mgr_description,
    .enabled = plugin_mgr_enabled,
    .set_enabled = plugin_mgr_set_enabled,
};
