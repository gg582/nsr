#ifndef NSR_TUI_H
#define NSR_TUI_H

#include <nsr/telemetry.h>
#include <nsr/state/topology.h>
#include <nsr/ui/key_slots.h>
#include <ttak/ttak_accelerator.h>
#include <time.h>
#include <time.h>

typedef enum {
    NSR_UI_NORMAL = 0,
    NSR_UI_GRID,
    NSR_UI_TREE,
    NSR_UI_TOOLS,
} nsr_ui_mode_t;

#define NSR_UI_STACK_DEPTH 8

typedef struct {
    nsr_ui_mode_t modes[NSR_UI_STACK_DEPTH];
    int count;
} nsr_ui_stack_t;

struct nsr_plugin_registry;
typedef struct nsr_plugin_registry nsr_plugin_registry_t;

typedef struct nsr_tui_state {
    nsr_ui_mode_t current_mode;
    nsr_ui_stack_t nav_stack;

    int grid_cursor;
    int tree_cursor;
    int tree_scroll;
    int grid_scroll_x;
    int grid_scroll_y;
    int tools_cursor;
    int tools_scroll;

    uint32_t focused_node_id;
    bool show_stats;
    bool show_dashboard;
    bool frozen;

    int last_max_y;
    int last_max_x;

    char msg[128];
    time_t msg_until;

    nsr_key_manager_t keys;
} nsr_tui_state_t;

/* ============================================================
 * Pseudo-OOP TUI driver (vtable + opaque this).
 * ============================================================ */

typedef struct nsr_tui_driver nsr_tui_driver_t;

struct nsr_tui_driver_vtable {
    ttak_result_t (*init)(nsr_tui_driver_t *self);
    void (*cleanup)(nsr_tui_driver_t *self);
    void (*render)(nsr_tui_driver_t *self,
                   nsr_telemetry_state_t *tel,
                   nsr_topology_state_t *topo,
                   nsr_plugin_manager_t *plugins);
    int (*update)(nsr_tui_driver_t *self,
                  nsr_topology_state_t *topo,
                  nsr_plugin_manager_t *plugins);
    void (*toggle_dashboard)(nsr_tui_driver_t *self);
};

struct nsr_tui_driver {
    const struct nsr_tui_driver_vtable *vt;
    nsr_tui_state_t state;
};

extern const struct nsr_tui_driver_vtable nsr_tui_driver_vtable;

/* Legacy free functions (still used by the driver internally). */
ttak_result_t nsr_tui_init(void);
void nsr_tui_cleanup(void);
void nsr_tui_render(nsr_tui_state_t *tui, nsr_telemetry_state_t *tel,
                    nsr_topology_state_t *topo, nsr_plugin_manager_t *plugins);
int nsr_tui_update(nsr_tui_state_t *tui, nsr_topology_state_t *topo,
                   nsr_plugin_manager_t *plugins);
void nsr_tui_toggle_dashboard(nsr_tui_state_t *tui);

#endif
