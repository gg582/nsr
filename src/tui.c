#ifndef _XOPEN_SOURCE_EXTENDED
#define _XOPEN_SOURCE_EXTENDED
#endif
#include <nsr/tui.h>
#include <ncursesw/curses.h>
#include <locale.h>
#include <ttak/timing/timing.h>
#include <ttak/math/bigint.h>
#include <ttak/mem/mem.h>

ttak_result_t nsr_tui_init(void) {
    setlocale(LC_ALL, "");
    initscr();
    start_color();
    use_default_colors();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);

    // Modern Color Palette
    init_pair(1, COLOR_CYAN, -1);    // Accent
    init_pair(2, COLOR_GREEN, -1);   // Success
    init_pair(3, COLOR_RED, -1);     // Error/Timeout
    init_pair(4, COLOR_YELLOW, -1);  // Pending
    init_pair(5, COLOR_WHITE, COLOR_BLUE); // Highlight
    
    return TTAK_RESULT_OK;
}

static void draw_box(int y, int x, int h, int w, const char *title) {
    attron(COLOR_PAIR(1));
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
    attroff(COLOR_PAIR(1));
}

static bool g_show_stats = false;

void nsr_tui_render(nsr_omni_state_t *state) {
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    erase();

    // 1. Header Area
    attron(COLOR_PAIR(1) | A_BOLD);
    mvprintw(0, 2, "NSR NET MUSHROOM v0.1.0 (OMNI)");
    attroff(COLOR_PAIR(1) | A_BOLD);
    
    // Timeline-based memory management: Bind object lifetimes to the system clock duration
    uint64_t now_ns = ttak_get_tick_count_ns();
    
    // Allocate BigInt objects with a explicit 1-second TTL on the timeline
    ttak_bigint_t *bi_now = (ttak_bigint_t *)ttak_mem_alloc_with_flags(sizeof(ttak_bigint_t), TT_SECOND(1), now_ns, TTAK_MEM_STRICT_CHECK);
    ttak_bigint_t *bi_start = (ttak_bigint_t *)ttak_mem_alloc_with_flags(sizeof(ttak_bigint_t), TT_SECOND(1), now_ns, TTAK_MEM_STRICT_CHECK);
    ttak_bigint_t *bi_diff = (ttak_bigint_t *)ttak_mem_alloc_with_flags(sizeof(ttak_bigint_t), TT_SECOND(1), now_ns, TTAK_MEM_STRICT_CHECK);
    ttak_bigint_t *bi_uptime = (ttak_bigint_t *)ttak_mem_alloc_with_flags(sizeof(ttak_bigint_t), TT_SECOND(1), now_ns, TTAK_MEM_STRICT_CHECK);

    if (bi_now && bi_start && bi_diff && bi_uptime) {
        ttak_bigint_init_u64(bi_now, now_ns / 1000, now_ns);
        ttak_bigint_init_u64(bi_start, state->start_time_us, now_ns);
        ttak_bigint_init(bi_diff, now_ns);
        ttak_bigint_init(bi_uptime, now_ns);

        // Safe subtraction handles precision and underflow via BigInt sign flag
        ttak_bigint_sub(bi_diff, bi_now, bi_start, now_ns);
        
        // Convert microseconds to seconds (1,000,000 scale)
        ttak_bigint_div_u64(bi_uptime, NULL, bi_diff, 1000000ULL, now_ns);

        char *uptime_str = ttak_bigint_to_string(bi_uptime, now_ns);
        if (uptime_str) {
            mvprintw(0, max_x - 25, "UPTIME: %s s", uptime_str);
        } else {
            mvprintw(0, max_x - 25, "UPTIME: [MEM_FAULT]");
        }
    } else {
        mvprintw(0, max_x - 25, "UPTIME: [TIMELINE_FAULT]");
    }
    
    // Trigger timeline cleanup for expired objects
    tt_autoclean_dirty_pointers(now_ns);

    // 2. Stats Panel
    draw_box(2, 2, 3, max_x - 5, "TARGET INFO");
    mvprintw(3, 4, "DESTINATION: %-40s", state->target_ip[0] ? state->target_ip : "Scanning...");
    if (g_show_stats) {
        attron(COLOR_PAIR(2));
        mvprintw(3, max_x - 30, "[STATS ACTIVE] SHM RING OK");
        attroff(COLOR_PAIR(2));
    }

    // 3. Trace Panel
    draw_box(6, 2, max_y - 9, max_x - 5, "TRACEROUTE");
    attron(A_UNDERLINE | A_BOLD);
    mvprintw(7, 4, "HOP  ADDRESS / STATUS          RTT      SENT  RECV  LOSS");
    attroff(A_UNDERLINE | A_BOLD);

    for (int i = 1; i < 16; i++) {
        nsr_hop_info_t *h = &state->hops[i];
        int row = 7 + i;
        if (row >= max_y - 3) break;

        mvprintw(row, 4, "%2d", i);
        
        float loss = h->sent ? (1.0f - (float)h->recv / h->sent) * 100.0f : 0.0f;

        if (h->sent == 0) {
            mvprintw(row, 9, "-");
            continue;
        }

        switch (h->last_status) {
            case NSR_OBS_REPLY:
            case NSR_OBS_EXCEEDED:
                attron(COLOR_PAIR(2));
                mvprintw(row, 9, "%-25s %4lu ms", h->addr[0] ? h->addr : "Reply Received", h->rtt_us / 1000);
                attroff(COLOR_PAIR(2));
                break;
            case NSR_OBS_TIMEOUT:
                attron(COLOR_PAIR(3));
                mvprintw(row, 9, "* * * Request Timed Out");
                attroff(COLOR_PAIR(3));
                break;
            default:
                attron(COLOR_PAIR(4));
                mvprintw(row, 9, "Probing...");
                attroff(COLOR_PAIR(4));
                break;
        }

        mvprintw(row, 44, "%4u  %4u  %3.0f%%", h->sent, h->recv, loss);
    }

    // 4. Footer
    attron(COLOR_PAIR(5));
    mvhline(max_y - 1, 0, ' ', max_x);
    mvprintw(max_y - 1, 2, "[Q] Quit  [S] Settings  [P] Pause (NSR Omni Physical Isolation Active)");
    attroff(COLOR_PAIR(5));

    refresh();
}

int nsr_tui_update(void) {
    int ch = getch();
    if (ch == 'q' || ch == 'Q') return 1;
    if (ch == 'p' || ch == 'P') return 2;
    if (ch == 's' || ch == 'S') {
        g_show_stats = !g_show_stats;
        return 3;
    }
    return 0;
}

void nsr_tui_cleanup(void) {
    endwin();
}
