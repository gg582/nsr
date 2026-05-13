#ifndef NSR_TUI_H
#define NSR_TUI_H

#include <nsr/telemetry.h>
#include <nsr/topology.h>
#include <ttak/ttak_accelerator.h>

typedef enum {
    NSR_UI_NORMAL = 0,
    NSR_UI_GRID,
    NSR_UI_TREE,
} nsr_ui_mode_t;

#define NSR_UI_STACK_DEPTH 8

typedef struct {
    nsr_ui_mode_t modes[NSR_UI_STACK_DEPTH];
    int count;
} nsr_ui_stack_t;

typedef struct {
    nsr_ui_mode_t current_mode;
    nsr_ui_stack_t nav_stack;

    int grid_cursor;
    int tree_cursor;
    int tree_scroll;
    int grid_scroll_x;
    int grid_scroll_y;

    uint32_t focused_node_id;
    bool show_stats;
    bool show_dashboard;

    int last_max_y;
    int last_max_x;
} nsr_tui_state_t;

ttak_result_t nsr_tui_init(void);
void nsr_tui_cleanup(void);

void nsr_tui_render(nsr_tui_state_t *tui, nsr_telemetry_state_t *tel, nsr_topology_state_t *topo);
int nsr_tui_update(nsr_tui_state_t *tui, nsr_topology_state_t *topo);

void nsr_tui_toggle_dashboard(nsr_tui_state_t *tui);

#endif
