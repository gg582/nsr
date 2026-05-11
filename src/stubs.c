#include <stdint.h>
/**
 * @brief Linker stubs to resolve platform-specific drivers in the CLI environment.
 * This allows us to measure the exact binary size of the feature-complete NSR.
 */
void ttak_net_driver_detect(void) {}
void ttak_net_driver_init(void) {}
