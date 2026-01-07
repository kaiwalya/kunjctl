#pragma once

#include "esp_err.h"

/**
 * One-time init/deinit (encodes Hello, stores device_id).
 */
esp_err_t comms_init(const char *device_id);
void comms_deinit(void);

/**
 * Open/close radio for each communication cycle.
 */
esp_err_t comms_open(void);
void comms_close(void);

/**
 * Send messages (call between open/close).
 */
esp_err_t comms_send_hello(void);
