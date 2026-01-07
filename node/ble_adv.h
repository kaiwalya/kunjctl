#pragma once

#include "esp_err.h"

/**
 * Initialize BLE stack.
 * Call once at startup.
 */
esp_err_t ble_adv_init(void);

/**
 * Run NimBLE event loop (BLOCKING).
 * Call this LAST in app_main - it never returns.
 * This turns app_main into the NimBLE task.
 */
void ble_adv_run(void);

/**
 * Start advertising (broadcasting).
 * Device will appear as "ESP32-H2" to nearby scanners.
 */
esp_err_t ble_adv_start(void);

/**
 * Stop advertising.
 */
esp_err_t ble_adv_stop(void);
