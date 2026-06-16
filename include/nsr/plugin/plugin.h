#ifndef NSR_PLUGIN_H
#define NSR_PLUGIN_H

#include <nsr/telemetry.h>
#include <nsr/state/topology.h>
#include <nsr/json/json_rpc.h>
#include <stdbool.h>

#define NSR_MAX_PLUGINS 32
#define NSR_PLUGIN_NAME_MAX 64
#define NSR_KEY_USER_LEN 32

/* Forward declaration to break the ui <-> plugin include cycle. */
struct nsr_tui_state;

typedef struct {
    char name[NSR_PLUGIN_NAME_MAX];
    char path[512];
    char description[256];
    bool enabled;
    bool initialized;
    bool dead;
    nsr_json_rpc_t rpc;
    /* Cached responses so TUI never blocks waiting for slow plugins. */
    nsr_json_buf_t last_render_resp;
    nsr_json_buf_t last_render_hops_resp;
    bool render_pending;
    bool render_hops_pending;
    bool on_key_pending;
    bool input_grabbed;
    bool is_modal;
    long long pending_render_id;
    long long pending_render_hops_id;
    long long pending_on_key_id;
    long long last_on_key_ms;
    /* Keys this plugin reserved at init time (lowercase, sorted). */
    char reserved_keys[NSR_KEY_USER_LEN];
    int reserved_count;

    /* Relaunch / crash monitoring */
    bool crash_countdown_active;
    long long crash_start_time;
} nsr_plugin_entry_t;

typedef struct nsr_plugin_registry {
    nsr_plugin_entry_t entries[NSR_MAX_PLUGINS];
    int count;
    char config_path[256];
    char plugin_dir[512];
} nsr_plugin_registry_t;

void nsr_plugins_init(nsr_plugin_registry_t *reg, const char *config_path);
void nsr_plugins_cleanup(nsr_plugin_registry_t *reg);

int nsr_plugins_load_dir(nsr_plugin_registry_t *reg, const char *dir);

void nsr_plugins_update_telemetry(nsr_plugin_registry_t *reg,
                                  const nsr_telemetry_state_t *tel,
                                  const nsr_topology_state_t *topo);
void nsr_plugins_render(nsr_plugin_registry_t *reg,
                        const struct nsr_tui_state *tui,
                        const nsr_topology_state_t *topo,
                        int y, int x, int h, int w,
                        int screen_h, int screen_w);
typedef struct {
    int hop_idx;
    char text[128];
} nsr_hop_annotation_t;

int nsr_plugins_render_hops(nsr_plugin_registry_t *reg,
                            const nsr_hop_info_t *hops,
                            int hop_count,
                            nsr_hop_annotation_t *out,
                            int max_out);
bool nsr_plugins_on_key(nsr_plugin_registry_t *reg, int ch);
bool nsr_plugins_modal_active(nsr_plugin_registry_t *reg);

int  nsr_plugins_count(const nsr_plugin_registry_t *reg);
const char *nsr_plugins_name(const nsr_plugin_registry_t *reg, int idx);
const char *nsr_plugins_description(const nsr_plugin_registry_t *reg, int idx);
bool nsr_plugins_enabled(const nsr_plugin_registry_t *reg, int idx);
void nsr_plugins_set_enabled(nsr_plugin_registry_t *reg, int idx, bool enabled);

/* ============================================================
 * Pseudo-OOP plugin manager (vtable + opaque this).
 * ============================================================ */

typedef struct nsr_plugin_manager nsr_plugin_manager_t;

struct nsr_plugin_manager_vtable {
    void (*init)(nsr_plugin_manager_t *self, const char *config_path);
    void (*cleanup)(nsr_plugin_manager_t *self);
    int  (*load_dir)(nsr_plugin_manager_t *self, const char *dir);
    void (*update_telemetry)(nsr_plugin_manager_t *self,
                             const nsr_telemetry_state_t *tel,
                             const nsr_topology_state_t *topo);
    void (*render)(nsr_plugin_manager_t *self,
                   const struct nsr_tui_state *tui,
                   const nsr_topology_state_t *topo,
                   int y, int x, int h, int w,
                   int screen_h, int screen_w);
    int  (*render_hops)(nsr_plugin_manager_t *self,
                        const nsr_hop_info_t *hops,
                        int hop_count,
                        nsr_hop_annotation_t *out,
                        int max_out);
    bool (*on_key)(nsr_plugin_manager_t *self, int ch);
    bool (*modal_active)(nsr_plugin_manager_t *self);
    int  (*count)(const nsr_plugin_manager_t *self);
    const char *(*name)(const nsr_plugin_manager_t *self, int idx);
    const char *(*description)(const nsr_plugin_manager_t *self, int idx);
    bool (*enabled)(const nsr_plugin_manager_t *self, int idx);
    void (*set_enabled)(nsr_plugin_manager_t *self, int idx, bool enabled);
};

struct nsr_plugin_manager {
    const struct nsr_plugin_manager_vtable *vt;
    nsr_plugin_registry_t registry;
};

extern const struct nsr_plugin_manager_vtable nsr_plugin_manager_vtable;

#endif
