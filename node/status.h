#pragma once

#include <stdint.h>

/**
 * Initialize status LED (if enabled via Kconfig)
 */
void status_init(void);

/**
 * Set status LED color. Values are 0-255 for each channel.
 * Different colors can indicate different states:
 *   - Red: busy/working
 *   - Green: success/idle
 *   - Blue: BLE activity
 *   - etc.
 */
void status_set(uint8_t r, uint8_t g, uint8_t b);

/**
 * Turn off status LED
 */
void status_off(void);
