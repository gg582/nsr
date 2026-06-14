#ifndef NSR_KEY_SLOTS_H
#define NSR_KEY_SLOTS_H

#include <nsr/plugin/plugin.h>
#include <stdbool.h>
#include <stdint.h>

/* Maximum number of reserved keys a single plugin may declare. */
#define NSR_KEY_USER_LEN 32

typedef struct nsr_key_manager nsr_key_manager_t;

struct nsr_key_manager_vtable {
    void (*init)(nsr_key_manager_t *self, nsr_plugin_manager_t *plugins);
    bool (*matches)(const nsr_key_manager_t *self, char original, int ch);
    char (*label_char)(const nsr_key_manager_t *self, char original);
};

/* Direct 256-entry LUT: index = original key ASCII, value = remapped key ASCII.
 * A value of 0 means "no remap".  This gives O(1) lookup without sorting or
 * binary search, which is optimal for the small alphabetic key space NSR uses. */
struct nsr_key_manager {
    const struct nsr_key_manager_vtable *vt;
    uint8_t remap[256];
};

extern const struct nsr_key_manager_vtable nsr_key_manager_vtable;

#endif
