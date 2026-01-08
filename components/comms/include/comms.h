#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/*── Types ──*/

typedef enum {
    COMMS_SOURCE_NODE = 0,
    COMMS_SOURCE_HUB = 1
} comms_source_t;

typedef struct {
    comms_source_t source;
} comms_hello_t;

/* For sending: pointers to optional values */
typedef struct {
    const float *temperature_c;
    const float *humidity_pct;
    const bool *relay_state;
} comms_report_t;

/* For receiving: inline storage, no pointer ownership issues */
typedef struct {
    bool has_temperature;
    float temperature_c;
    bool has_humidity;
    float humidity_pct;
    bool has_relay;
    bool relay_state;
} comms_report_data_t;

typedef struct {
    char device_id[32];
    uint32_t message_id;

    bool has_hello;
    comms_hello_t hello;

    bool has_report;
    comms_report_data_t report;
} comms_message_t;

/*── Lifecycle ──*/

esp_err_t comms_init(const char *device_id, comms_source_t source);
void comms_deinit(void);
esp_err_t comms_open(void);
void comms_close(void);

/*── Sending ──*/

esp_err_t comms_send_hello_for(uint32_t duration_ms);
esp_err_t comms_send_report_for(const comms_report_t *report, uint32_t duration_ms);

/*── Receiving ──*/

/**
 * Callback for received messages.
 * @param msg  Received message (valid only during callback)
 */
typedef void (*comms_message_cb_t)(const comms_message_t *msg);

/**
 * Start continuous scanning (for hub).
 * Messages delivered via callback as they arrive.
 */
esp_err_t comms_start_scanning(comms_message_cb_t callback);
void comms_stop_scanning(void);

/**
 * Scan for messages for the specified duration (for node).
 * Dedupes by message_id.
 * @param duration_ms  How long to scan
 * @param out          Buffer to store received messages
 * @param max_count    Size of buffer
 * @return Number of unique messages received (may be 0)
 */
int comms_scan_for(uint32_t duration_ms, comms_message_t *out, int max_count);
