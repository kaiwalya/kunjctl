#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_bt.h"
#include "power_management.h"
#include "status.h"

/* NimBLE */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"

static const char *TAG = "node";
static volatile bool ble_synced = false;

#define BLE_SCAN_DURATION_MS   3000
#define BLE_SCAN_INTERVAL_MS   60000
#define PM_STATS_INTERVAL_MS   10000

/* Called for each advertisement discovered */
static int on_scan_event(struct ble_gap_event *event, void *arg) {
    if (event->type == BLE_GAP_EVENT_DISC) {
        struct ble_gap_disc_desc *desc = &event->disc;

        /* Extract device name from advertisement data */
        char name[32] = "(unknown)";
        struct ble_hs_adv_fields fields;
        if (ble_hs_adv_parse_fields(&fields, desc->data, desc->length_data) == 0) {
            if (fields.name != NULL && fields.name_len > 0) {
                int len = fields.name_len < sizeof(name) - 1 ? fields.name_len : sizeof(name) - 1;
                memcpy(name, fields.name, len);
                name[len] = '\0';
            }
        }

        ESP_LOGI(TAG, "Found: %02X:%02X:%02X:%02X:%02X:%02X  RSSI: %d dBm  Name: %s",
                 desc->addr.val[5], desc->addr.val[4], desc->addr.val[3],
                 desc->addr.val[2], desc->addr.val[1], desc->addr.val[0],
                 desc->rssi, name);
    }
    return 0;
}

static void scan_for_devices(int duration_ms) {
    struct ble_gap_disc_params scan_params = {
        .itvl = 160,
        .window = 80,
        .filter_policy = 0,
        .limited = 0,
        .passive = 1,
        .filter_duplicates = 1,
    };

    ESP_LOGI(TAG, "Scanning for %d ms...", duration_ms);

    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, duration_ms, &scan_params, on_scan_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start scan: %d", rc);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(duration_ms + 100));
}

static void on_ble_sync(void) {
    ESP_LOGI(TAG, "BLE synced");
    ble_synced = true;
}

static void nimble_host_task(void *arg) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_init(void) {
    ble_synced = false;

    nimble_port_init();
    ble_hs_cfg.sync_cb = on_ble_sync;
    nimble_port_freertos_init(nimble_host_task);

    /* Wait for sync */
    while (!ble_synced) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void ble_deinit(void) {
    int rc = nimble_port_stop();
    if (rc != 0) {
        ESP_LOGE(TAG, "nimble_port_stop failed: %d", rc);
    }
    nimble_port_deinit();
}

void app_main(void)
{
    /* Initialize NVS once (required for BLE) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* Initialize status LED */
    status_init();

    /* Initialize power management */
    pm_config_t pm_cfg = {
        .num_wake_gpios = 0,
        .light_sleep_enable = true,
        .stats_interval_ms = PM_STATS_INTERVAL_MS,
    };
    pm_init(&pm_cfg);

    ESP_LOGI(TAG, "Starting scan loop (scan %dms every %dms)",
             BLE_SCAN_DURATION_MS, BLE_SCAN_INTERVAL_MS);

    /* Main loop: init, scan, deinit, sleep */
    for (;;) {
        status_set(16, 0, 0);  /* Red = scanning */
        ble_init();
        scan_for_devices(BLE_SCAN_DURATION_MS);
        ble_deinit();
        status_off();

        ESP_LOGI(TAG, "Sleeping for %d seconds...", BLE_SCAN_INTERVAL_MS / 1000);
        vTaskDelay(pdMS_TO_TICKS(BLE_SCAN_INTERVAL_MS));
    }
}
