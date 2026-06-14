#include <nsr/ui/key_slots.h>
#include <string.h>
#include <ctype.h>

/* NSR built-in shortcut keys (caseless). Each letter is one default binding. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
constexpr const char *RESERVED_KEYS = "qngmtzspd";
#else
static const char * const RESERVED_KEYS = "qngmtzspd";
#endif

static char nsr_tolower_char(char c)
{
    if (c >= 'A' && c <= 'Z')
        return (char)(c + ('a' - 'A'));
    return c;
}

static char nsr_toupper_char(char c)
{
    if (c >= 'a' && c <= 'z')
        return (char)(c - ('a' - 'A'));
    return c;
}

static void key_mgr_init(nsr_key_manager_t *self, nsr_plugin_manager_t *plugins)
{
    memset(self->remap, 0, sizeof(self->remap));

    if (!plugins)
        return;

    /* Pool of fallback keys for reassignment. */
    const char *slots = "QWERTYUIOP";
    int slot_idx = 0;

    for (int i = 0; i < plugins->registry.count; i++) {
        nsr_plugin_entry_t *e = &plugins->registry.entries[i];
        if (!e->enabled || !e->initialized || e->dead)
            continue;
        for (int k = 0; k < e->reserved_count; k++) {
            char rk = e->reserved_keys[k];

            /* Does it conflict with a built-in NSR key? */
            bool conflict = false;
            for (const char *p = RESERVED_KEYS; *p; p++) {
                if (nsr_tolower_char(*p) == rk) {
                    conflict = true;
                    break;
                }
            }
            if (!conflict)
                continue;

            /* Already remapped by another plugin? */
            if (self->remap[(unsigned char)rk] != 0)
                continue;

            if (slots[slot_idx] == '\0')
                continue;

            char assigned = slots[slot_idx++];
            self->remap[(unsigned char)rk] = (uint8_t)assigned;
            self->remap[(unsigned char)nsr_toupper_char(rk)] = (uint8_t)assigned;
        }
    }
}

static bool key_mgr_matches(const nsr_key_manager_t *self, char original, int ch)
{
    if (ch < 0 || ch > 255)
        return false;
    uint8_t mapped = self->remap[(unsigned char)original];
    char target = mapped ? (char)mapped : original;
    return nsr_tolower_char((char)ch) == nsr_tolower_char(target);
}

static char key_mgr_label_char(const nsr_key_manager_t *self, char original)
{
    uint8_t mapped = self->remap[(unsigned char)original];
    return mapped ? (char)mapped : original;
}

const struct nsr_key_manager_vtable nsr_key_manager_vtable = {
    .init = key_mgr_init,
    .matches = key_mgr_matches,
    .label_char = key_mgr_label_char,
};
