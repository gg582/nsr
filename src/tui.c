#ifndef _XOPEN_SOURCE_EXTENDED
#define _XOPEN_SOURCE_EXTENDED
#endif
#include <nsr/tui.h>
#include <ncursesw/curses.h>
#include <locale.h>
#include <ttak/timing/timing.h>
#include <ttak/math/bigint.h>
#include <ttak/mem/mem.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

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
        mvprintw(y, x + 2, " %s ", title);
        attroff(A_BOLD);
    }
    attroff(COLOR_PAIR(CP_ACCENT));
}

static void draw_footer(nsr_tui_state_t *tui, int max_y, int max_x)
{
    (void)tui;
    attron(COLOR_PAIR(CP_HIGHLIGHT));
    mvhline(max_y - 1, 0, ' ', max_x);
    mvprintw(max_y - 1, 2,
             "[n] Normal  [g] Grid  [t] Tree  [h/j/k/l] Move  [Enter] Focus  [c] Control-Plane  [Esc] Back  [q] Quit");
    if (tui->show_dashboard) {
        mvprintw(max_y - 1, max_x - 18, "[D] Close Settings");
    } else {
        mvprintw(max_y - 1, max_x - 18, "[D] Settings");
    }
    attroff(COLOR_PAIR(CP_HIGHLIGHT));
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
    return NULL;
}

static nsr_topology_node_t *g_tree_nodes[NSR_TOPOLOGY_MAX_NODES];
static int g_tree_node_count = 0;

static nsr_topology_node_t *tree_node_by_index(int index)
{
    if (index < 0 || index >= g_tree_node_count)
        return NULL;
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

static void render_normal(nsr_tui_state_t *tui, nsr_telemetry_state_t *state)
{
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    erase();

    attron(COLOR_PAIR(CP_ACCENT) | A_BOLD);
    mvprintw(0, 2, "NSR NET MUSHROOM v0.1.0");
    attroff(COLOR_PAIR(CP_ACCENT) | A_BOLD);

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
        ttak_bigint_div_u64(bi_uptime, NULL, bi_diff, 1000000ULL, now_ns);
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
    tt_autoclean_dirty_pointers(now_ns);

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
    }

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
        mvprintw(y + 4, x + 2, "[+/-] Minimum Round Duration (-i): %d ms", state->interval_ms);
        mvprintw(y + 6, x + 2, "Press [D] to close");
    }

    draw_footer(tui, max_y, max_x);
    refresh();
}

static void render_grid(nsr_tui_state_t *tui, nsr_topology_state_t *topo)
{
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    erase();

    attron(COLOR_PAIR(CP_ACCENT) | A_BOLD);
    mvprintw(0, 2, "TOPOLOGY GRID");
    attroff(COLOR_PAIR(CP_ACCENT) | A_BOLD);

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

    draw_footer(tui, max_y, max_x);
    refresh();
}

static void render_tree(nsr_tui_state_t *tui, nsr_topology_state_t *topo)
{
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    erase();

    attron(COLOR_PAIR(CP_ACCENT) | A_BOLD);
    mvprintw(0, 2, "TOPOLOGY TREE");
    attroff(COLOR_PAIR(CP_ACCENT) | A_BOLD);

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

    int line_idx = 0;
    int cursor_idx = 0;

    for (int t = 0; t < target_count; t++) {
        /* Target header (not selectable). */
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

                    char label[128];
                    snprintf(label, sizeof(label), "%c%s%s  rtt:%4.1fms loss:%3.0f%%",
                             cp_mark, dest_mark, tnodes[i]->addr,
                             tnodes[i]->health.rtt_us / 1000.0f,
                             tnodes[i]->health.loss_rate * 100.0f);
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

    draw_footer(tui, max_y, max_x);
    refresh();
}

void nsr_tui_render(nsr_tui_state_t *tui, nsr_telemetry_state_t *tel, nsr_topology_state_t *topo)
{
    switch (tui->current_mode) {
    case NSR_UI_GRID:
        render_grid(tui, topo);
        break;
    case NSR_UI_TREE:
        render_tree(tui, topo);
        break;
    default:
        render_normal(tui, tel);
        break;
    }
}

int nsr_tui_update(nsr_tui_state_t *tui, nsr_topology_state_t *topo)
{
    int ch = getch();
    if (ch == 'q' || ch == 'Q')
        return 1;

    if (ch == 'n' || ch == 'N') {
        goto_normal(tui);
        return 0;
    }
    if (ch == 'g' || ch == 'G') {
        if (tui->current_mode != NSR_UI_GRID)
            push_nav(tui, NSR_UI_GRID);
        return 0;
    }
    if (ch == 't' || ch == 'T') {
        if (tui->current_mode != NSR_UI_TREE)
            push_nav(tui, NSR_UI_TREE);
        return 0;
    }
    if (ch == 27) {
        pop_nav(tui);
        return 0;
    }

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
        if (ch == 'p' || ch == 'P')
            return 2;
        if (ch == 's' || ch == 'S') {
            tui->show_stats = !tui->show_stats;
            return 3;
        }
        if (ch == '+' || ch == '=')
            return 4;
        if (ch == '-' || ch == '_')
            return 5;
        if (ch == 'd' || ch == 'D') {
            tui->show_dashboard = !tui->show_dashboard;
            return 6;
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
