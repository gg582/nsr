#ifndef NSR_TUI_H
#define NSR_TUI_H

#include <nsr/omni.h>
#include <ttak/ttak_accelerator.h>

/**
 * @brief Initializes the ncursesw interface.
 */
ttak_result_t nsr_tui_init(void);

/**
 * @brief Renders the current tracer state to the screen.
 */
void nsr_tui_render(nsr_omni_state_t *state);

/**
 * @brief Cleans up and restores the terminal.
 */
void nsr_tui_cleanup(void);

/**
 * @brief Handles user input (non-blocking).
 * @return true if the app should quit.
 */
bool nsr_tui_update(void);

#endif
