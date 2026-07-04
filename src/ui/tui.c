#ifndef _XOPEN_SOURCE_EXTENDED
#define _XOPEN_SOURCE_EXTENDED
#endif
#include <nsr/ui/tui.h>
#include <nsr/plugin/plugin.h>
#include <nsr/json/json.h>
#include <ncursesw/curses.h>
#include <locale.h>
#include <ttak/timing/timing.h>
#include <ttak/math/bigint.h>
#include <ttak/mem/mem.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <time.h>

enum {
    CP_ACCENT = 1,
    CP_HEALTH_GOOD,
    CP_HEALTH_DEGRADED,
    CP_HEALTH_POOR,
    CP_HEALTH_CRITICAL,
    CP_HEALTH_UNREACHABLE,
    CP_CTRL_PLANE,
    CP_HIGHLIGHT,
};

static void tui_print_clipped(int y, int x, int width, const char *text)
{
    if (y < 0 || x < 0 || width <= 0 || !text)
        return;
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    if (y >= max_y || x >= max_x)
        return;
    if (x + width > max_x)
        width = max_x - x;
    if (width <= 0)
        return;
    mvprintw(y, x, "%.*s", width, text);
}

ttak_result_t nsr_tui_init(void)
{
    setlocale(LC_ALL, "");
    initscr();
    start_color();
    use_default_colors();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);

    init_pair(CP_ACCENT, COLOR_CYAN, -1);
    init_pair(CP_HEALTH_GOOD, COLOR_GREEN, -1);
    init_pair(CP_HEALTH_DEGRADED, COLOR_YELLOW, -1);
    init_pair(CP_HEALTH_POOR, COLOR_RED, -1);
    init_pair(CP_HEALTH_CRITICAL, COLOR_RED, -1);
    init_pair(CP_HEALTH_UNREACHABLE, COLOR_MAGENTA, -1);
    init_pair(CP_CTRL_PLANE, COLOR_BLUE, -1);
    init_pair(CP_HIGHLIGHT, COLOR_WHITE, COLOR_BLUE);

    return TTAK_RESULT_OK;
}

static void draw_box(int y, int x, int h, int w, const char *title)
{
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    if (y < 0 || x < 0 || h < 1 || w < 1 || y >= max_y || x >= max_x)
        return;
    if (y + h >= max_y)
        h = max_y - y - 1;
    if (x + w >= max_x)
        w = max_x - x - 1;
    if (h < 1 || w < 1)
        return;

    attron(COLOR_PAIR(CP_ACCENT));
    mvhline(y, x, 0, w);
    mvhline(y + h, x, 0, w);
    mvvline(y, x, 0, h);
    mvvline(y, x + w, 0, h);
    mvaddch(y, x, ACS_ULCORNER);
    mvaddch(y, x + w, ACS_URCORNER);
    mvaddch(y + h, x, ACS_LLCORNER);
    mvaddch(y + h, x + w, ACS_LRCORNER);
    if (title) {
        attron(A_BOLD);
        tui_print_clipped(y, x + 2, w - 3, title);
        attroff(A_BOLD);
    }
    attroff(COLOR_PAIR(CP_ACCENT));
}

static void draw_footer(nsr_tui_state_t *tui, nsr_plugin_manager_t *plugins, int max_y, int max_x)
{
    if (!tui || max_y < 1 || max_x < 4)
        return;

    attron(COLOR_PAIR(CP_HIGHLIGHT));
    mvhline(max_y - 1, 0, ' ', max_x);

    bool has_modal = false;
    bool sniffer_enabled = false;
    if (plugins) {
        for (int i = 0; i < plugins->registry.count; i++) {
            nsr_plugin_entry_t *e = &plugins->registry.entries[i];
            if (strcmp(e->name, "sniffer") == 0) {
                sniffer_enabled = e->enabled;
            }
            if (e->enabled && e->initialized && !e->dead && e->last_render_resp.len > 0) {
                size_t mlen;
                const char *res = nsr_json_obj_get(nsr_json_cstr(&e->last_render_resp), "result", &mlen);
                if (res) {
                    const char *mv = nsr_json_obj_get(res, "is_modal", &mlen);
                    if (mv) {
                        bool im = false;
                        nsr_json_parse_bool(mv, mlen, &im);
                        if (im) has_modal = true;
                    }
                }
            }
        }
    }

    char buf[512];
    if (has_modal) {
        snprintf(buf, sizeof(buf),
                 "[Up/Down] Focus Field  [Left/Right] Protocol  [Enter][Enter] Apply & Close  [Esc] Cancel");
        tui_print_clipped(max_y - 1, 2, max_x - 4, buf);
    } else {
        if (max_x < 110) {
            snprintf(buf, sizeof(buf),
                     "[%c] Normal  [%c] Grid  [%c] Tools  [%c] Tree  [%c] Freeze  [%c] Quit",
                     tui->keys.vt->label_char(&tui->keys, 'n'),
                     tui->keys.vt->label_char(&tui->keys, 'g'),
                     tui->keys.vt->label_char(&tui->keys, 'm'),
                     tui->keys.vt->label_char(&tui->keys, 't'),
                     tui->keys.vt->label_char(&tui->keys, 'p'),
                     tui->keys.vt->label_char(&tui->keys, 'q'));
        } else {
            snprintf(buf, sizeof(buf),
                     "[%c] Normal  [%c] Grid  [%c] Tools  [%c] Tree  [h/j/k/l] Move  [Enter] Focus  [%c] Freeze  [%c] Quit",
                     tui->keys.vt->label_char(&tui->keys, 'n'),
                     tui->keys.vt->label_char(&tui->keys, 'g'),
                     tui->keys.vt->label_char(&tui->keys, 'm'),
                     tui->keys.vt->label_char(&tui->keys, 't'),
                     tui->keys.vt->label_char(&tui->keys, 'p'),
                     tui->keys.vt->label_char(&tui->keys, 'q'));
        }

        if (sniffer_enabled) {
            int len = (int)strlen(buf);
            if (max_x - len > 50) {
                strncat(buf, "  [a] Inspect settings  [s] Last packet  [f] Sniff", sizeof(buf) - len - 1);
            }
        }

        int right_w = max_x >= 22 ? 20 : 0;
        int left_w = max_x - right_w - 4;
        tui_print_clipped(max_y - 1, 2, left_w, buf);

        if (right_w > 0) {
            if (tui->show_dashboard) {
                snprintf(buf, sizeof(buf), "[[%c] Close Settings", tui->keys.vt->label_char(&tui->keys, 'd'));
            } else {
                snprintf(buf, sizeof(buf), "[[%c] Settings", tui->keys.vt->label_char(&tui->keys, 'd'));
            }
            tui_print_clipped(max_y - 1, max_x - right_w, right_w, buf);
        }
    }
    attroff(COLOR_PAIR(CP_HIGHLIGHT));
}

static void show_tui_msg(nsr_tui_state_t *tui, const char *text, int seconds)
{
    strncpy(tui->msg, text, sizeof(tui->msg) - 1);
    tui->msg[sizeof(tui->msg) - 1] = '\0';
    tui->msg_until = time(nullptr) + seconds;
}

static void draw_notice(nsr_tui_state_t *tui, int max_y, int max_x)
{
    if (tui->msg[0] == '\0' || time(nullptr) >= tui->msg_until) {
        tui->msg[0] = '\0';
        return;
    }
    int len = (int)strlen(tui->msg);
    int w = len + 4;
    int h = 3;
    if (w > max_x)
        w = max_x;
    if (w < 5)
        w = 5;
    int x = (max_x - w) / 2;
    int y = (max_y - h) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    for (int i = 0; i < h; i++)
        mvhline(y + i, x, ' ', w);
    draw_box(y, x, h, w, nullptr);
    attron(A_BOLD);
    mvprintw(y + 1, x + 2, "%.*s", w - 4, tui->msg);
    attroff(A_BOLD);
}

static void push_nav(nsr_tui_state_t *tui, nsr_ui_mode_t mode)
{
    if (tui->nav_stack.count < NSR_UI_STACK_DEPTH) {
        tui->nav_stack.modes[tui->nav_stack.count++] = tui->current_mode;
    }
    tui->current_mode = mode;
}

static nsr_ui_mode_t pop_nav(nsr_tui_state_t *tui)
{
    if (tui->nav_stack.count > 0) {
        tui->current_mode = tui->nav_stack.modes[--tui->nav_stack.count];
    } else {
        tui->current_mode = NSR_UI_NORMAL;
    }
    return tui->current_mode;
}

static void goto_normal(nsr_tui_state_t *tui)
{
    tui->nav_stack.count = 0;
    tui->current_mode = NSR_UI_NORMAL;
}

static nsr_topology_node_t *node_by_visual_index(nsr_topology_state_t *topo, int index)
{
    int idx = 0;
    for (int i = 0; i < NSR_TOPOLOGY_MAX_NODES; i++) {
        nsr_topology_node_t *n = &topo->nodes[i];
        if (!n->active)
            continue;
        if (idx == index)
            return n;
        idx++;
    }
    return nullptr;
}

static nsr_topology_node_t *g_tree_nodes[NSR_TOPOLOGY_MAX_NODES];
static int g_tree_node_count = 0;

static nsr_topology_node_t *tree_node_by_index(int index)
{
    if (index < 0 || index >= g_tree_node_count)
        return nullptr;
    return g_tree_nodes[index];
}

static int health_pair_for_node(const nsr_topology_node_t *node)
{
    float score = nsr_health_score(&node->health);
    if (score < 0.15f)
        return CP_HEALTH_GOOD;
    if (score < 0.35f)
        return CP_HEALTH_DEGRADED;
    if (score < 0.60f)
        return CP_HEALTH_POOR;
    if (score < 0.85f)
        return CP_HEALTH_CRITICAL;
    return CP_HEALTH_UNREACHABLE;
}

static void render_normal(nsr_tui_state_t *tui, nsr_telemetry_state_t *state,
                          nsr_topology_state_t *topo,
                          nsr_plugin_manager_t *plugins)
{
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    erase();

    attron(COLOR_PAIR(CP_ACCENT) | A_BOLD);
    mvprintw(0, 2, "NSR: the Network Diagnosis Tool");
    attroff(COLOR_PAIR(CP_ACCENT) | A_BOLD);

    if (tui->frozen) {
        attron(COLOR_PAIR(CP_HEALTH_CRITICAL) | A_BOLD);
        mvprintw(0, max_x - 14, "[FROZEN]");
        attroff(COLOR_PAIR(CP_HEALTH_CRITICAL) | A_BOLD);
    }

    uint64_t now_ns = ttak_get_tick_count_ns();

    ttak_bigint_t *bi_now = (ttak_bigint_t *)ttak_mem_alloc_with_flags(
        sizeof(ttak_bigint_t), TT_SECOND(1), now_ns, TTAK_MEM_STRICT_CHECK);
    ttak_bigint_t *bi_start = (ttak_bigint_t *)ttak_mem_alloc_with_flags(
        sizeof(ttak_bigint_t), TT_SECOND(1), now_ns, TTAK_MEM_STRICT_CHECK);
    ttak_bigint_t *bi_diff = (ttak_bigint_t *)ttak_mem_alloc_with_flags(
        sizeof(ttak_bigint_t), TT_SECOND(1), now_ns, TTAK_MEM_STRICT_CHECK);
    ttak_bigint_t *bi_uptime = (ttak_bigint_t *)ttak_mem_alloc_with_flags(
        sizeof(ttak_bigint_t), TT_SECOND(1), now_ns, TTAK_MEM_STRICT_CHECK);

    if (bi_now && bi_start && bi_diff && bi_uptime) {
        ttak_bigint_init_u64(bi_now, now_ns / 1000, now_ns);
        ttak_bigint_init_u64(bi_start, state->start_time_us, now_ns);
        ttak_bigint_init(bi_diff, now_ns);
        ttak_bigint_init(bi_uptime, now_ns);
        ttak_bigint_sub(bi_diff, bi_now, bi_start, now_ns);
        ttak_bigint_div_u64(bi_uptime, nullptr, bi_diff, 1000000ULL, now_ns);
        char *uptime_str = ttak_bigint_to_string(bi_uptime, now_ns);
        if (uptime_str) {
            mvprintw(0, max_x - 45, "INTERVAL: %d ms  UPTIME: %s s",
                     state->interval_ms, uptime_str);
        } else {
            mvprintw(0, max_x - 45, "INTERVAL: %d ms  UPTIME: [MEM_FAULT]", state->interval_ms);
        }
    } else {
        mvprintw(0, max_x - 45, "INTERVAL: %d ms  UPTIME: [TIMELINE_FAULT]", state->interval_ms);
    }
    ttak_mem_alloc_with_flags(0, 0, 0, 0);

    draw_box(2, 2, 3, max_x - 5, "TARGET INFO");
    mvprintw(3, 4, "DESTINATION: %-40s",
             state->target_ip[0] ? state->target_ip : "Scanning...");
    if (tui->show_stats) {
        attron(COLOR_PAIR(CP_HEALTH_GOOD));
        mvprintw(3, max_x - 30, "[STATS ACTIVE] SHM RING OK");
        attroff(COLOR_PAIR(CP_HEALTH_GOOD));
    }

    draw_box(6, 2, max_y - 9, max_x - 5, "TRACEROUTE");
    attron(A_UNDERLINE | A_BOLD);
    mvprintw(7, 4, "HOP  ADDRESS / STATUS          RTT      SENT  RECV  LOSS");
    attroff(A_UNDERLINE | A_BOLD);

    nsr_hop_annotation_t annotations[NSR_MAX_HOPS];
    int ann_count = 0;
    if (plugins)
        ann_count = plugins->vt->render_hops(plugins, state->hops, NSR_MAX_HOPS,
                                            annotations, NSR_MAX_HOPS);

    for (int i = 1; i < NSR_MAX_HOPS; i++) {
        nsr_hop_info_t *h = &state->hops[i];
        int row = 7 + i;
        if (row >= max_y - 3)
            break;

        mvprintw(row, 4, "%2d", i);
        float loss = h->sent ? (1.0f - (float)h->recv / h->sent) * 100.0f : 0.0f;

        if (h->sent == 0) {
            mvprintw(row, 9, "-");
            continue;
        }

        switch (h->last_status) {
        case NSR_OBS_REPLY:
        case NSR_OBS_EXCEEDED:
        case NSR_OBS_UNREACH:
            attron(COLOR_PAIR(CP_HEALTH_GOOD));
            if (h->alias_count > 0) {
                mvprintw(row, 9, "%-21s +%-2u %4lu ms",
                         h->addr[0] ? h->addr : "Reply Received",
                         h->alias_count,
                         h->rtt_us / 1000);
            } else {
                mvprintw(row, 9, "%-25s %4lu ms",
                         h->addr[0] ? h->addr : "Reply Received",
                         h->rtt_us / 1000);
            }
            attroff(COLOR_PAIR(CP_HEALTH_GOOD));
            break;
        case NSR_OBS_TIMEOUT:
            attron(COLOR_PAIR(CP_HEALTH_CRITICAL));
            mvprintw(row, 9, "* * * Request Timed Out");
            attroff(COLOR_PAIR(CP_HEALTH_CRITICAL));
            break;
        default:
            attron(COLOR_PAIR(CP_HEALTH_DEGRADED));
            mvprintw(row, 9, "Probing...");
            attroff(COLOR_PAIR(CP_HEALTH_DEGRADED));
            break;
        }
        mvprintw(row, 44, "%4u  %4u  %3.0f%%", h->sent, h->recv, loss);

        for (int a = 0; a < ann_count; a++) {
            if (annotations[a].hop_idx == i) {
                mvprintw(row, 56, "%s", annotations[a].text);
                break;
            }
        }
    }

    if (plugins)
        plugins->vt->render(plugins, tui, topo, 9, max_x - 55, max_y - 12, 38,
                            max_y, max_x);

    if (tui->show_dashboard) {
        int w = 50;
        int h = 10;
        int y = (max_y - h) / 2;
        int x = (max_x - w) / 2;
        for (int i = 0; i < h; i++) {
            mvhline(y + i, x, ' ', w);
        }
        draw_box(y, x, h, w, "SETTINGS DASHBOARD");
        attron(A_BOLD);
        mvprintw(y + 2, x + 2, "Tracer Settings:");
        attroff(A_BOLD);
        mvprintw(y + 4, x + 2, "[j/k] Minimum Round Duration (-i): %d ms", state->interval_ms);
        mvprintw(y + 6, x + 2, "Press [%c] to close", tui->keys.vt->label_char(&tui->keys, 'd'));
    }

    draw_footer(tui, plugins, max_y, max_x);
    refresh();
}

static void render_grid(nsr_tui_state_t *tui, nsr_topology_state_t *topo,
                        nsr_plugin_manager_t *plugins)
{
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    erase();

    attron(COLOR_PAIR(CP_ACCENT) | A_BOLD);
    mvprintw(0, 2, "TOPOLOGY GRID");
    attroff(COLOR_PAIR(CP_ACCENT) | A_BOLD);

    if (tui->frozen) {
        attron(COLOR_PAIR(CP_HEALTH_CRITICAL) | A_BOLD);
        mvprintw(0, max_x - 14, "[FROZEN]");
        attroff(COLOR_PAIR(CP_HEALTH_CRITICAL) | A_BOLD);
    }

    int cols = max_x / 14;
    if (cols < 1)
        cols = 1;
    int start_x = 2;
    int start_y = 2;
    int avail_rows = max_y - start_y - 2;

    int total = nsr_topology_active_count(topo);
    if (tui->current_mode == NSR_UI_TREE)
        total = g_tree_node_count;
    int rows = (total + cols - 1) / cols;
    if (rows < 1)
        rows = 1;

    if (tui->grid_cursor < 0)
        tui->grid_cursor = 0;
    if (tui->grid_cursor >= total)
        tui->grid_cursor = total - 1;
    if (tui->grid_cursor < 0)
        tui->grid_cursor = 0;

    int cursor_row = tui->grid_cursor / cols;

    if (cursor_row < tui->grid_scroll_y)
        tui->grid_scroll_y = cursor_row;
    if (cursor_row >= tui->grid_scroll_y + avail_rows)
        tui->grid_scroll_y = cursor_row - avail_rows + 1;
    if (tui->grid_scroll_y < 0)
        tui->grid_scroll_y = 0;
    if (rows > avail_rows && tui->grid_scroll_y > rows - avail_rows)
        tui->grid_scroll_y = rows - avail_rows;
    if (tui->grid_scroll_y < 0)
        tui->grid_scroll_y = 0;

    int idx = 0;
    for (int i = 0; i < NSR_TOPOLOGY_MAX_NODES && idx < total; i++) {
        const nsr_topology_node_t *node = &topo->nodes[i];
        if (!node->active)
            continue;
        int row = idx / cols;
        int col = idx % cols;
        idx++;

        if (row < tui->grid_scroll_y || row >= tui->grid_scroll_y + avail_rows)
            continue;

        int y = start_y + (row - tui->grid_scroll_y);
        int x = start_x + col * 14;

        bool is_cursor = (idx - 1 == tui->grid_cursor);
        int pair = health_pair_for_node(node);
        if (is_cursor)
            attron(COLOR_PAIR(CP_HIGHLIGHT));
        else
            attron(COLOR_PAIR(pair));

        char label[14];
        snprintf(label, sizeof(label), "%-12s", node->addr);
        mvprintw(y, x, "%s", label);

        if (atomic_load(&node->control_plane))
            mvaddch(y, x + 12, '*');

        if (is_cursor)
            attroff(COLOR_PAIR(CP_HIGHLIGHT));
        else
            attroff(COLOR_PAIR(pair));
    }

    if (total == 0)
        mvprintw(start_y, start_x, "No nodes discovered yet.");

    if (plugins)
        plugins->vt->render(plugins, tui, topo, 9, max_x - 55, max_y - 12, 38,
                            max_y, max_x);

    draw_footer(tui, plugins, max_y, max_x);
    refresh();
}

static void render_tree(nsr_tui_state_t *tui, nsr_topology_state_t *topo,
                        nsr_plugin_manager_t *plugins)
{
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    erase();

    attron(COLOR_PAIR(CP_ACCENT) | A_BOLD);
    mvprintw(0, 2, "TOPOLOGY TREE");
    attroff(COLOR_PAIR(CP_ACCENT) | A_BOLD);

    if (tui->frozen) {
        attron(COLOR_PAIR(CP_HEALTH_CRITICAL) | A_BOLD);
        mvprintw(0, max_x - 14, "[FROZEN]");
        attroff(COLOR_PAIR(CP_HEALTH_CRITICAL) | A_BOLD);
    }

    int start_y = 2;
    int start_x = 2;
    int avail_rows = max_y - start_y - 2;

    g_tree_node_count = 0;

    char targets[NSR_MAX_TARGETS][NSR_NODE_ADDR_LEN];
    int target_count = 0;
    for (int i = 0; i < NSR_TOPOLOGY_MAX_NODES; i++) {
        nsr_topology_node_t *n = &topo->nodes[i];
        if (!n->active) continue;
        bool found = false;
        for (int t = 0; t < target_count; t++) {
            if (strcmp(targets[t], n->target_ip) == 0) { found = true; break; }
        }
        if (!found && target_count < NSR_MAX_TARGETS) {
            strncpy(targets[target_count++], n->target_ip, NSR_NODE_ADDR_LEN - 1);
        }
    }

    nsr_hop_info_t temp_hops[NSR_MAX_HOPS];
    memset(temp_hops, 0, sizeof(temp_hops));
    int temp_hop_count = 1;

    int sim_line_idx = 0;
    for (int t = 0; t < target_count; t++) {
        sim_line_idx++;

        nsr_topology_node_t *tnodes[NSR_TOPOLOGY_MAX_NODES];
        int tnode_count = 0;
        for (int i = 0; i < NSR_TOPOLOGY_MAX_NODES; i++) {
            nsr_topology_node_t *n = &topo->nodes[i];
            if (n->active && strcmp(n->target_ip, targets[t]) == 0) {
                tnodes[tnode_count++] = n;
            }
        }
        if (tnode_count == 0) continue;

        for (int i = 1; i < tnode_count; i++) {
            nsr_topology_node_t *key = tnodes[i];
            int j = i - 1;
            while (j >= 0 && tnodes[j]->ttl > key->ttl) {
                tnodes[j + 1] = tnodes[j];
                j--;
            }
            tnodes[j + 1] = key;
        }

        for (int i = 0; i < tnode_count; i++) {
            if (sim_line_idx >= tui->tree_scroll && sim_line_idx < tui->tree_scroll + avail_rows) {
                if (temp_hop_count < NSR_MAX_HOPS) {
                    nsr_hop_info_t *th = &temp_hops[temp_hop_count];
                    strncpy(th->addr, tnodes[i]->addr, sizeof(th->addr) - 1);
                    th->addr[sizeof(th->addr) - 1] = '\0';
                    th->rtt_us = (uint64_t)tnodes[i]->health.rtt_us;
                    th->sent = tnodes[i]->health.sent > 0 ? tnodes[i]->health.sent : 1;
                    th->recv = tnodes[i]->health.recv;
                    th->last_status = NSR_OBS_REPLY;
                    temp_hop_count++;
                }
            }
            sim_line_idx++;
        }
    }

    nsr_hop_annotation_t annotations[NSR_MAX_HOPS];
    int ann_count = 0;
    if (plugins && temp_hop_count > 1) {
        ann_count = plugins->vt->render_hops(plugins, temp_hops, temp_hop_count,
                                             annotations, NSR_MAX_HOPS);
    }

    int line_idx = 0;
    int cursor_idx = 0;

    for (int t = 0; t < target_count; t++) {
        if (line_idx >= tui->tree_scroll && line_idx < tui->tree_scroll + avail_rows) {
            int y = start_y + (line_idx - tui->tree_scroll);
            if (y < max_y - 2) {
                attron(COLOR_PAIR(CP_ACCENT) | A_BOLD);
                mvprintw(y, start_x, "# %s", targets[t]);
                attroff(COLOR_PAIR(CP_ACCENT) | A_BOLD);
            }
        }
        line_idx++;

        nsr_topology_node_t *tnodes[NSR_TOPOLOGY_MAX_NODES];
        int tnode_count = 0;
        for (int i = 0; i < NSR_TOPOLOGY_MAX_NODES; i++) {
            nsr_topology_node_t *n = &topo->nodes[i];
            if (n->active && strcmp(n->target_ip, targets[t]) == 0) {
                tnodes[tnode_count++] = n;
            }
        }
        if (tnode_count == 0) continue;

        for (int i = 1; i < tnode_count; i++) {
            nsr_topology_node_t *key = tnodes[i];
            int j = i - 1;
            while (j >= 0 && tnodes[j]->ttl > key->ttl) {
                tnodes[j + 1] = tnodes[j];
                j--;
            }
            tnodes[j + 1] = key;
        }

        int ttl_count[256] = {0};
        for (int i = 0; i < tnode_count; i++) ttl_count[tnodes[i]->ttl]++;
        int ttl_seen[256] = {0};

        for (int i = 0; i < tnode_count && g_tree_node_count < NSR_TOPOLOGY_MAX_NODES; i++) {
            g_tree_nodes[g_tree_node_count++] = tnodes[i];

            int ttl = tnodes[i]->ttl;
            bool is_last = (ttl_seen[ttl] == ttl_count[ttl] - 1);
            ttl_seen[ttl]++;

            if (line_idx >= tui->tree_scroll && line_idx < tui->tree_scroll + avail_rows) {
                int y = start_y + (line_idx - tui->tree_scroll);
                if (y < max_y - 2) {
                    bool is_cursor = (cursor_idx == tui->tree_cursor);
                    int pair = health_pair_for_node(tnodes[i]);

                    if (is_cursor)
                        attron(COLOR_PAIR(CP_HIGHLIGHT));
                    else
                        attron(COLOR_PAIR(pair));

                    char prefix[256] = "";
                    int depth = ttl;
                    if (depth > 15) depth = 15;
                    for (int d = 1; d < depth; d++) {
                        strcat(prefix, "│  ");
                    }
                    if (depth > 0) {
                        strcat(prefix, is_last ? "└── " : "├── ");
                    }

                    char cp_mark = atomic_load(&tnodes[i]->control_plane) ? '*' : ' ';
                    char dest_mark[16] = "";
                    if (tnodes[i]->is_destination)
                        snprintf(dest_mark, sizeof(dest_mark), "[DEST] ");

                    int prefix_len = (int)strlen(prefix);
                    int remain = max_x - start_x - prefix_len - 2;
                    if (remain < 10) remain = 10;

                    char label[256];
                    snprintf(label, sizeof(label), "%c%s%s  rtt:%4.1fms loss:%3.0f%%",
                             cp_mark, dest_mark, tnodes[i]->addr,
                             tnodes[i]->health.rtt_us / 1000.0f,
                             tnodes[i]->health.loss_rate * 100.0f);

                    for (int a = 0; a < ann_count; a++) {
                        int h_idx = annotations[a].hop_idx;
                        if (h_idx > 0 && h_idx < temp_hop_count && strcmp(temp_hops[h_idx].addr, tnodes[i]->addr) == 0) {
                            size_t cur_len = strlen(label);
                            snprintf(label + cur_len, sizeof(label) - cur_len, "  %s", annotations[a].text);
                            break;
                        }
                    }

                    if ((int)strlen(label) > remain)
                        label[remain] = '\0';

                    mvprintw(y, start_x, "%s%s", prefix, label);

                    if (is_cursor)
                        attroff(COLOR_PAIR(CP_HIGHLIGHT));
                    else
                        attroff(COLOR_PAIR(pair));
                }
            }
            line_idx++;
            cursor_idx++;
        }
    }

    int total = g_tree_node_count;
    if (tui->tree_cursor < 0) tui->tree_cursor = 0;
    if (tui->tree_cursor >= total) tui->tree_cursor = total - 1;
    if (tui->tree_cursor < 0) tui->tree_cursor = 0;

    if (tui->tree_cursor < tui->tree_scroll)
        tui->tree_scroll = tui->tree_cursor;
    if (tui->tree_cursor >= tui->tree_scroll + avail_rows)
        tui->tree_scroll = tui->tree_cursor - avail_rows + 1;
    if (tui->tree_scroll < 0)
        tui->tree_scroll = 0;

    if (total == 0)
        mvprintw(start_y, start_x, "No nodes discovered yet.");

    if (plugins)
        plugins->vt->render(plugins, tui, topo, 9, max_x - 55, max_y - 12, 38,
                            max_y, max_x);

    draw_footer(tui, plugins, max_y, max_x);
    refresh();
}

static void render_tools(nsr_tui_state_t *tui, nsr_plugin_manager_t *plugins)
{
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    erase();

    attron(COLOR_PAIR(CP_ACCENT) | A_BOLD);
    mvprintw(0, 2, "TOOLS MENU");
    attroff(COLOR_PAIR(CP_ACCENT) | A_BOLD);

    int w = 64;
    int h = max_y - 8;
    if (h < 10) h = 10;
    int y = (max_y - h) / 2;
    int x = (max_x - w) / 2;

    for (int i = 0; i < h; i++)
        mvhline(y + i, x, ' ', w);
    draw_box(y, x, h, w, "PLUGINS");

    int count = plugins ? plugins->vt->count(plugins) : 0;

    if (tui->tools_cursor < 0) tui->tools_cursor = 0;
    if (count > 0 && tui->tools_cursor >= count) tui->tools_cursor = count - 1;
    if (tui->tools_cursor < 0) tui->tools_cursor = 0;

    int avail = h - 4;
    if (tui->tools_cursor < tui->tools_scroll)
        tui->tools_scroll = tui->tools_cursor;
    if (tui->tools_cursor >= tui->tools_scroll + avail)
        tui->tools_scroll = tui->tools_cursor - avail + 1;
    if (tui->tools_scroll < 0)
        tui->tools_scroll = 0;

    if (count == 0) {
        mvprintw(y + 2, x + 2, "No plugins loaded.");
    } else {
        for (int i = 0; i < avail && i + tui->tools_scroll < count; i++) {
            int idx = i + tui->tools_scroll;
            int row = y + 2 + i;
            bool sel = (idx == tui->tools_cursor);
            if (sel)
                attron(COLOR_PAIR(CP_HIGHLIGHT));
            mvprintw(row, x + 2, "[%c] %-30s",
                     plugins->vt->enabled(plugins, idx) ? 'x' : ' ',
                     plugins->vt->name(plugins, idx));
            if (sel)
                attroff(COLOR_PAIR(CP_HIGHLIGHT));
        }

        int sel = tui->tools_cursor;
        if (sel >= 0 && sel < count) {
            const char *desc = plugins->vt->description(plugins, sel);
            if (desc) {
                attron(A_BOLD);
                mvprintw(y + h - 2, x + 2, "%% %s", desc);
                attroff(A_BOLD);
            }
        }
    }

    mvprintw(max_y - 3, x, "[j/k] Move  [Enter] Toggle  [Esc/q] Back");

    draw_footer(tui, plugins, max_y, max_x);
    refresh();
}

void nsr_tui_render(nsr_tui_state_t *tui, nsr_telemetry_state_t *tel,
                    nsr_topology_state_t *topo, nsr_plugin_manager_t *plugins)
{
    if (tui->frozen && tui->current_mode != NSR_UI_TOOLS) {
        int max_y, max_x;
        getmaxyx(stdscr, max_y, max_x);
        (void)max_y;
        attron(COLOR_PAIR(CP_HEALTH_CRITICAL) | A_BOLD);
        mvprintw(0, max_x - 14, "[FROZEN]");
        attroff(COLOR_PAIR(CP_HEALTH_CRITICAL) | A_BOLD);
        refresh();
        return;
    }

    switch (tui->current_mode) {
    case NSR_UI_GRID:
        render_grid(tui, topo, plugins);
        break;
    case NSR_UI_TREE:
        render_tree(tui, topo, plugins);
        break;
    case NSR_UI_TOOLS:
        render_tools(tui, plugins);
        break;
    default:
        render_normal(tui, tel, topo, plugins);
        break;
    }

    {
        int max_y, max_x;
        getmaxyx(stdscr, max_y, max_x);
        draw_notice(tui, max_y, max_x);
        refresh();
    }
}

int nsr_tui_update(nsr_tui_state_t *tui, nsr_topology_state_t *topo,
                   nsr_plugin_manager_t *plugins)
{
    int ch = getch();
    if (ch == ERR)
        return 0;

    if (plugins && plugins->vt->modal_active && plugins->vt->modal_active(plugins)) {
        plugins->vt->on_key(plugins, ch);
        return 0;
    }

    if (tui->keys.vt->matches(&tui->keys, 'q', ch))
        return 1;
    if (tui->keys.vt->matches(&tui->keys, 'd', ch)) {
        if (tui->current_mode != NSR_UI_NORMAL) {
            show_tui_msg(tui, "Please enter a dashboard at Normal Mode", 3);
            return 0;
        }
        return 6;
    }

    if (tui->current_mode == NSR_UI_TOOLS) {
        int count = plugins ? plugins->vt->count(plugins) : 0;
        switch (ch) {
        case 'k':
        case KEY_UP:
            if (tui->tools_cursor > 0) tui->tools_cursor--;
            break;
        case 'j':
        case KEY_DOWN:
            if (tui->tools_cursor < count - 1) tui->tools_cursor++;
            break;
        case 'h':
        case KEY_LEFT:
            if (tui->tools_cursor > 0) tui->tools_cursor -= 5;
            break;
        case 'l':
        case KEY_RIGHT:
            if (tui->tools_cursor < count - 1) tui->tools_cursor += 5;
            break;
        case '\n':
        case '\r':
        case KEY_ENTER:
            if (plugins && tui->tools_cursor >= 0 && tui->tools_cursor < count) {
                bool en = plugins->vt->enabled(plugins, tui->tools_cursor);
                plugins->vt->set_enabled(plugins, tui->tools_cursor, !en);
            }
            break;
        case 27:
        case 't':
        case 'T':
            if (ch == 27 || tui->keys.vt->matches(&tui->keys, 't', ch))
                pop_nav(tui);
            break;
        }
        if (tui->tools_cursor < 0) tui->tools_cursor = 0;
        if (tui->tools_cursor >= count) tui->tools_cursor = count > 0 ? count - 1 : 0;
        return 0;
    }

    if (tui->keys.vt->matches(&tui->keys, 'n', ch)) {
        goto_normal(tui);
        return 0;
    }
    if (tui->keys.vt->matches(&tui->keys, 'g', ch)) {
        if (tui->current_mode != NSR_UI_GRID)
            push_nav(tui, NSR_UI_GRID);
        return 0;
    }
    if (tui->keys.vt->matches(&tui->keys, 'm', ch)) {
        if (tui->current_mode != NSR_UI_TOOLS)
            push_nav(tui, NSR_UI_TOOLS);
        return 0;
    }
    if (tui->keys.vt->matches(&tui->keys, 't', ch)) {
        if (tui->current_mode != NSR_UI_TREE)
            push_nav(tui, NSR_UI_TREE);
        return 0;
    }
    if (ch == 27) {
        pop_nav(tui);
        return 0;
    }
    if (tui->keys.vt->matches(&tui->keys, 'p', ch)) {
        tui->frozen = !tui->frozen;
        return 0;
    }

    if (plugins && plugins->vt->on_key(plugins, ch))
        return 0;

    int total = nsr_topology_active_count(topo);

    if (tui->current_mode == NSR_UI_GRID) {
        int cols = COLS / 14;
        if (cols < 1)
            cols = 1;
        switch (ch) {
        case 'h':
        case KEY_LEFT:
            tui->grid_cursor--;
            break;
        case 'l':
        case KEY_RIGHT:
            tui->grid_cursor++;
            break;
        case 'k':
        case KEY_UP:
            tui->grid_cursor -= cols;
            break;
        case 'j':
        case KEY_DOWN:
            tui->grid_cursor += cols;
            break;
        case '\n':
        case KEY_ENTER:
            if (tui->grid_cursor >= 0 && tui->grid_cursor < total) {
                nsr_topology_node_t *n = node_by_visual_index(topo, tui->grid_cursor);
                if (n)
                    tui->focused_node_id = n->id;
                push_nav(tui, NSR_UI_NORMAL);
            }
            return 0;
        case 'c':
        case 'C': {
            nsr_topology_node_t *n = node_by_visual_index(topo, tui->grid_cursor);
            if (n) {
                bool cp = atomic_load(&n->control_plane);
                atomic_store(&n->control_plane, !cp);
            }
            return 0;
        }
        }
        if (tui->grid_cursor < 0)
            tui->grid_cursor = 0;
        if (tui->grid_cursor >= total)
            tui->grid_cursor = total - 1;
    } else if (tui->current_mode == NSR_UI_TREE) {
        switch (ch) {
        case 'k':
        case KEY_UP:
            if (tui->tree_cursor > 0)
                tui->tree_cursor--;
            break;
        case 'j':
        case KEY_DOWN:
            if (tui->tree_cursor < total - 1)
                tui->tree_cursor++;
            break;
        case 'h':
        case KEY_LEFT:
            if (tui->tree_cursor > 0)
                tui->tree_cursor -= 5;
            break;
        case 'l':
        case KEY_RIGHT:
            if (tui->tree_cursor < total - 1)
                tui->tree_cursor += 5;
            break;
        case '\n':
        case KEY_ENTER:
            if (tui->tree_cursor >= 0 && tui->tree_cursor < total) {
                nsr_topology_node_t *n = tree_node_by_index(tui->tree_cursor);
                if (n)
                    tui->focused_node_id = n->id;
                push_nav(tui, NSR_UI_NORMAL);
            }
            return 0;
        case 'c':
        case 'C': {
            nsr_topology_node_t *n = tree_node_by_index(tui->tree_cursor);
            if (n) {
                bool cp = atomic_load(&n->control_plane);
                atomic_store(&n->control_plane, !cp);
            }
            return 0;
        }
        }
        if (tui->tree_cursor < 0)
            tui->tree_cursor = 0;
        if (tui->tree_cursor >= total)
            tui->tree_cursor = total - 1;
    } else {
        if (tui->keys.vt->matches(&tui->keys, 'z', ch))
            return 2;
        if (ch == 'S') {
            tui->show_stats = !tui->show_stats;
            return 3;
        }
        if (tui->show_dashboard) {
            if (ch == 'k')
                return 4;
            if (ch == 'j')
                return 5;
        }
    }
    return 0;
}

void nsr_tui_toggle_dashboard(nsr_tui_state_t *tui)
{
    tui->show_dashboard = !tui->show_dashboard;
}

void nsr_tui_cleanup(void)
{
    endwin();
}

/* ============================================================
 * Pseudo-OOP TUI driver implementation.
 * ============================================================ */

static ttak_result_t tui_driver_init(nsr_tui_driver_t *self)
{
    (void)self;
    return nsr_tui_init();
}

static void tui_driver_cleanup(nsr_tui_driver_t *self)
{
    (void)self;
    nsr_tui_cleanup();
}

static void tui_driver_render(nsr_tui_driver_t *self,
                              nsr_telemetry_state_t *tel,
                              nsr_topology_state_t *topo,
                              nsr_plugin_manager_t *plugins)
{
    nsr_tui_render(&self->state, tel, topo, plugins);
}

static int tui_driver_update(nsr_tui_driver_t *self,
                             nsr_topology_state_t *topo,
                             nsr_plugin_manager_t *plugins)
{
    return nsr_tui_update(&self->state, topo, plugins);
}

static void tui_driver_toggle_dashboard(nsr_tui_driver_t *self)
{
    nsr_tui_toggle_dashboard(&self->state);
}

const struct nsr_tui_driver_vtable nsr_tui_driver_vtable = {
    .init = tui_driver_init,
    .cleanup = tui_driver_cleanup,
    .render = tui_driver_render,
    .update = tui_driver_update,
    .toggle_dashboard = tui_driver_toggle_dashboard,
};
