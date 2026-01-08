#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "power_management.h"
#include "device_name.h"
#include "comms.h"
#include <string.h>

static const char *TAG = "hub";

#define PM_STATS_INTERVAL_MS        60000
#define SCAN_INTERVAL_MS            100
#define HELLO_RESPONSE_PRE_WAIT_MS  1000 // make sure node is out of broadcast and in scan mode
#define HELLO_RESPONSE_MS           2000
#define RELAY_CMD_MS                2000
#define MAX_MESSAGES                16

/*── Message Deduplication ──*/

#define SEEN_IDS_SIZE 32
static uint32_t seen_ids[SEEN_IDS_SIZE];
static int seen_ids_idx = 0;

static bool already_seen(uint32_t id) {
    for (int i = 0; i < SEEN_IDS_SIZE; i++) {
        if (seen_ids[i] == id) return true;
    }
    seen_ids[seen_ids_idx] = id;
    seen_ids_idx = (seen_ids_idx + 1) % SEEN_IDS_SIZE;
    return false;
}

/*── Message Collection ──*/

static comms_message_t g_messages[MAX_MESSAGES];
static int g_message_count = 0;

static void on_message(const comms_message_t *msg) {
    if (already_seen(msg->message_id)) return;

    if (g_message_count >= MAX_MESSAGES) {
        ESP_LOGW(TAG, "Message buffer full, dropping from %s", msg->device_id);
        return;
    }

    g_messages[g_message_count++] = *msg;
}

void app_main(void)
{
    char device_name[32];
    device_name_get(device_name, sizeof(device_name));
    ESP_LOGI(TAG, "Booting hub %s", device_name);

    /* Initialize power management (DFS only, no light sleep) */
    pm_config_t pm_cfg = {
        .num_wake_gpios = 0,
        .light_sleep_enable = false,
        .stats_interval_ms = PM_STATS_INTERVAL_MS,
    };
    pm_init(&pm_cfg);

    /* Initialize NVS (required for BLE) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* Initialize comms */
    if (comms_init(device_name, COMMS_SOURCE_HUB) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init comms");
        return;
    }

    if (comms_open() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open comms");
        return;
    }

    /* Start scanning */
    if (comms_start_scanning(on_message) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start scanning");
        return;
    }

    ESP_LOGI(TAG, "Hub ready, scanning...");

    /* Main loop */
    for (;;) {
        if (g_message_count != 0) {
            /* Process collected messages */
            for (int i = 0; i < g_message_count; i++) {
                comms_message_t *msg = &g_messages[i];

                if (msg->has_hello && msg->hello.source == COMMS_SOURCE_NODE) {
                    ESP_LOGI(TAG, "Hello from node: %s, responding...", msg->device_id);
                    comms_stop_scanning();
                    vTaskDelay(pdMS_TO_TICKS(HELLO_RESPONSE_PRE_WAIT_MS));
                    comms_send_hello_for(HELLO_RESPONSE_MS);
                    comms_start_scanning(on_message);
                }

                if (msg->has_report) {
                    ESP_LOGI(TAG, "Report from %s:", msg->device_id);
                    if (msg->report.has_temperature) {
                        ESP_LOGI(TAG, "  Temperature: %.1f C", msg->report.temperature_c);
                    }
                    if (msg->report.has_humidity) {
                        ESP_LOGI(TAG, "  Humidity: %.1f %%", msg->report.humidity_pct);
                    }
                    if (msg->report.has_relay) {
                        ESP_LOGI(TAG, "  Relay: %s", msg->report.relay_state ? "ON" : "OFF");
                        //flip relay state for demo purposes
                        comms_relay_cmd_t cmd = {
                            .relay_id = 0,
                            .state = !msg->report.relay_state
                        };
                        strncpy(cmd.device_id, msg->device_id, sizeof(cmd.device_id) - 1);
                        cmd.device_id[sizeof(cmd.device_id) - 1] = '\0';

                        ESP_LOGI(TAG, "Sending relay command to %s: %s", cmd.device_id, cmd.state ? "ON" : "OFF");
                        comms_stop_scanning();
                        comms_send_relay_cmd_for(&cmd, RELAY_CMD_MS);
                        comms_start_scanning(on_message);
                    }
                }
            }
            g_message_count = 0;
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(SCAN_INTERVAL_MS));
    }
}
