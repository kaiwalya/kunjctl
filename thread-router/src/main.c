#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_vfs_eventfd.h"
#include "nvs_flash.h"

#include "device_name.h"
#include "power_management.h"
#include "thread_comms.h"

static const char *TAG = "thread-router";

#define PM_STATS_INTERVAL_MS 60000

/**
 * Handle incoming messages from thread_comms
 */
static void on_thread_message(const thread_comms_message_t *msg)
{
    if (msg->type == THREAD_COMMS_MSG_REPORT) {
        const thread_comms_report_t *r = &msg->report;
        ESP_LOGI(TAG, "Report from '%s': temp=%.1f humidity=%.1f%% relay=%s",
                 r->device_id,
                 r->has_temperature ? r->temperature : 0,
                 r->has_humidity ? r->humidity : 0,
                 r->has_relay_state ? (r->relay_state ? "ON" : "OFF") : "N/A");

        /* Demo: invert relay state when we receive a report */
        if (r->has_relay_state) {
            thread_comms_relay_cmd_t cmd = {0};
            strncpy(cmd.device_id, r->device_id, sizeof(cmd.device_id) - 1);
            cmd.relay_state = !r->relay_state;

            ESP_LOGI(TAG, "Sending relay command to '%s': %s",
                     cmd.device_id, cmd.relay_state ? "ON" : "OFF");
            thread_comms_send_relay_cmd(&cmd);
        }
    }
}

void app_main(void)
{
    char device_name[32];
    device_name_get(device_name, sizeof(device_name));
    ESP_LOGI(TAG, "Thread Router - %s", device_name);

    /* Power management */
    pm_config_t pm_cfg = {
        .light_sleep_enable = false,
        .stats_interval_ms = PM_STATS_INTERVAL_MS,
    };
    pm_init(&pm_cfg);

    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* ESP-IDF networking stack */
    esp_vfs_eventfd_config_t eventfd_config = { .max_fds = 3 };
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_config));
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Thread networking and comms */
    thread_comms_set_callback(on_thread_message);
    ESP_ERROR_CHECK(thread_comms_init(device_name, THREAD_COMMS_SOURCE_ROUTER));

    ESP_LOGI(TAG, "Router running - listening for sensor reports...");

    /* Keep running */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
