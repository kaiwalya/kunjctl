/**
 * Minimal BLE Advertising Example
 *
 * BLE advertising = broadcasting small packets (up to 31 bytes) that any
 * nearby device can receive WITHOUT connecting.
 *
 * Think of it like a radio beacon - you're just shouting data into the air.
 */

#include "ble_adv.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include <string.h>

/* NimBLE headers */
#include "nimble/nimble_port.h"           // Initialize NimBLE stack
#include "nimble/nimble_port_freertos.h"  // Run NimBLE in a FreeRTOS task
#include "host/ble_hs.h"                  // BLE Host Stack core
#include "host/ble_gap.h"                 // GAP = advertising, scanning, connections
#include "services/gap/ble_svc_gap.h"     // GAP service (device name)
#include "os/os_mbuf.h"                   // Memory buffers for extended adv

static const char *TAG = "ble";

/* Store our address type (public or random) */
static uint8_t own_addr_type;

/* Forward declaration */
esp_err_t ble_adv_start(void);

/**
 * Called when the BLE stack is synchronized and ready.
 * This is where we can safely start using BLE functions.
 */
static void on_sync(void) {
    /* Determine what address type we have (public or random) */
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to determine address type: %d", rc);
        return;
    }

    /* Print our BLE MAC address */
    uint8_t addr[6];
    ble_hs_id_copy_addr(own_addr_type, addr, NULL);
    ESP_LOGI(TAG, "BLE Address: %02X:%02X:%02X:%02X:%02X:%02X",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);

    /* Auto-start advertising now that we're synced */
    ble_adv_start();
}

/**
 * Initialize NimBLE stack.
 * Must be called once at startup.
 */
esp_err_t ble_adv_init(void) {
    /* NVS is required for storing BLE bonding info */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) return ret;

    /* Initialize the NimBLE stack */
    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Set the sync callback - called when stack is ready */
    ble_hs_cfg.sync_cb = on_sync;

    /* Set the device name (visible to scanners) */
    ble_svc_gap_device_name_set("ESP32-H2");

    ESP_LOGI(TAG, "BLE initialized");
    return ESP_OK;
}

/**
 * Run the NimBLE event loop.
 * WARNING: This blocks forever! Call this last in app_main.
 */
void ble_adv_run(void) {
    ESP_LOGI(TAG, "Starting NimBLE event loop (blocking)...");
    nimble_port_run();  // Never returns
}

/**
 * Start extended advertising with 2M PHY.
 *
 * Extended advertising advantages:
 * - Supports 2M PHY (faster, lower power)
 * - Supports Coded PHY (long range)
 * - Larger payload (up to 255 bytes vs 31)
 *
 * 2M PHY benefits:
 * - 2x faster transmission = less time on air
 * - Lower power consumption
 * - Trade-off: shorter range (~50m vs ~100m for 1M)
 */
esp_err_t ble_adv_start(void) {
    int rc;
    static uint8_t ext_adv_instance = 0;

    /*
     * Extended advertising parameters
     *
     * legacy_pdu options:
     *   1 = Legacy format (compatible with all scanners, PHY ignored)
     *   0 = Extended format (BLE 5.0 only, enables 2M/Coded PHY)
     */
    struct ble_gap_ext_adv_params ext_params = {0};

    ext_params.connectable = 0;
    ext_params.scannable = 0;
    ext_params.legacy_pdu = 0;         // Extended mode for 2M PHY
    ext_params.itvl_min = 160;         // 100ms
    ext_params.itvl_max = 320;         // 200ms
    ext_params.primary_phy = BLE_HCI_LE_PHY_1M;
    ext_params.secondary_phy = BLE_HCI_LE_PHY_2M;
    ext_params.own_addr_type = own_addr_type;
    ext_params.sid = 0;
    ext_params.channel_map = 0x07;     // Use all 3 advertising channels (37, 38, 39)
    ext_params.tx_power = 127;         // 127 = no preference, use max available

    /* Configure the extended advertising instance */
    rc = ble_gap_ext_adv_configure(ext_adv_instance, &ext_params, NULL, NULL, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_ext_adv_configure failed: %d", rc);
        return ESP_FAIL;
    }

    /*
     * Build advertisement data
     * Format: [Length][Type][Data]...
     */
    const char *name = ble_svc_gap_device_name();
    uint8_t adv_data[31];
    int adv_len = 0;

    /* Flags: General discoverable, BR/EDR not supported */
    adv_data[adv_len++] = 2;                    // Length
    adv_data[adv_len++] = BLE_HS_ADV_TYPE_FLAGS;
    adv_data[adv_len++] = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    /* Complete local name */
    uint8_t name_len = strlen(name);
    adv_data[adv_len++] = name_len + 1;         // Length
    adv_data[adv_len++] = BLE_HS_ADV_TYPE_COMP_NAME;
    memcpy(&adv_data[adv_len], name, name_len);
    adv_len += name_len;

    /* Set the advertisement data */
    struct os_mbuf *data = os_msys_get_pkthdr(adv_len, 0);
    if (!data) {
        ESP_LOGE(TAG, "Failed to allocate mbuf");
        return ESP_FAIL;
    }
    os_mbuf_append(data, adv_data, adv_len);

    rc = ble_gap_ext_adv_set_data(ext_adv_instance, data);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_ext_adv_set_data failed: %d", rc);
        return ESP_FAIL;
    }

    /* Start advertising (0 = forever, 0 = no max events) */
    rc = ble_gap_ext_adv_start(ext_adv_instance, 0, 0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_ext_adv_start failed: %d", rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Extended advertising started (2M PHY) - look for 'ESP32-H2'");
    return ESP_OK;
}

/**
 * Stop advertising.
 */
esp_err_t ble_adv_stop(void) {
    int rc = ble_gap_ext_adv_stop(0);  // Instance 0
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_ext_adv_stop failed: %d", rc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Advertising stopped");
    return ESP_OK;
}
