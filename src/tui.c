#define _XOPEN_SOURCE_EXTENDED
#include <nsr/tui.h>
#include <ncursesw/curses.h>
#include <locale.h>
#include <ttak/timing/timing.h>

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

void nsr_tui_render(nsr_omni_state_t *state) {
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    erase();

    // 1. Header Area
    attron(COLOR_PAIR(1) | A_BOLD);
    mvprintw(0, 2, "NSR NET MUSHROOM v0.1.0 (OMNI)");
    attroff(COLOR_PAIR(1) | A_BOLD);
    
    uint64_t uptime_s = (ttak_get_tick_count_ns() / 1000 - state->start_time_us) / 1000000;
    mvprintw(0, max_x - 25, "UPTIME: %lu s", uptime_s);

    // 2. Stats Panel
    draw_box(2, 2, 3, max_x - 5, "TARGET INFO");
    mvprintw(3, 4, "DESTINATION: %s", state->target_ip[0] ? state->target_ip : "Scanning...");

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

bool nsr_tui_update(void) {
    int ch = getch();
    if (ch == 'q' || ch == 'Q') return true;
    return false;
}

void nsr_tui_cleanup(void) {
    endwin();
}
