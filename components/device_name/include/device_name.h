#pragma once

#include <stddef.h>

/**
 * Get deterministic device name based on chip ID.
 * Format: "{adjective}-{noun}-{hex}"
 * Example: "swift-falcon-a3f2"
 *
 * Same device always returns same name.
 *
 * @param buf Buffer to write name into
 * @param len Buffer size (recommend 32+ bytes)
 */
void device_name_get(char *buf, size_t len);
