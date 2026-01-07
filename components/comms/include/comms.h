#pragma once

#include "esp_err.h"
#include <stdint.h>

/**
 * Public mirror of internal SourceType enum.
 */
typedef enum {
    COMMS_SOURCE_NODE = 0,
    COMMS_SOURCE_HUB = 1
} comms_source_t;

/**
 * Public mirror of internal Hello message.
 */
typedef struct {
    comms_source_t source;
    char device_id[32];
} comms_hello_t;

/**
 * Callback for receiving Hello messages.
 * @param hello Pointer to the decoded Hello message
 */
typedef void (*comms_hello_cb_t)(const comms_hello_t *hello);

/**
 * One-time init/deinit (encodes Hello, stores device_id and source).
 */
esp_err_t comms_init(const char *device_id, comms_source_t source);
void comms_deinit(void);

/**
 * Open/close radio for each communication cycle.
 */
esp_err_t comms_open(void);
void comms_close(void);

/**
 * Node: Send Hello via BLE extended advertising (single event).
 * Call between open/close. Blocks until complete.
 */
esp_err_t comms_send_hello(void);

/**
 * Node: Send Hello for specified duration (blocking).
 * Call between open/close.
 */
esp_err_t comms_send_hello_for(uint32_t duration_ms);

/**
 * Hub: Start continuous BLE scanning for Hello messages.
 * Callback is invoked for each Hello received.
 * Call after comms_open().
 */
esp_err_t comms_start_scanning(comms_hello_cb_t callback);

/**
 * Hub: Stop BLE scanning.
 * Call before comms_close().
 */
void comms_stop_scanning(void);
