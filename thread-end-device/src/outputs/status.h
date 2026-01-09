#pragma once

#include <stdbool.h>

/**
 * Initialize status LED (if enabled via Kconfig)
 */
void status_init(void);

/**
 * Set busy status. When busy, LED shows red. When not busy, LED is off.
 */
void status_set_busy(bool busy);

/**
 * Blink green LED 3 times to indicate success.
 */
void status_it_worked(void);
