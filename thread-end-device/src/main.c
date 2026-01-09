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
#include "sensors.h"
#include "status.h"
#include "relay.h"
#include "thread_comms.h"

static const char *TAG = "thread-end-device";

static relay_t *g_relay = NULL;
static char g_device_name[32];

#define PM_STATS_INTERVAL_MS 60000

/**
 * Handle incoming relay commands from thread_comms
 */
static void on_thread_message(const thread_comms_message_t *msg)
{
    if (msg->type != THREAD_COMMS_MSG_RELAY_CMD) {
        return;
    }

    /* Check if this command is for us */
    if (strcmp(msg->relay_cmd.device_id, g_device_name) != 0) {
        return;
    }

    ESP_LOGI(TAG, "Received relay command: %s", msg->relay_cmd.relay_state ? "ON" : "OFF");
    if (g_relay != NULL) {
        relay_set(g_relay, msg->relay_cmd.relay_state);
    }
}

#if CONFIG_FACTORY_RESET_BUTTON_ENABLED
static void on_factory_reset_button(gpio_num_t gpio)
{
    ESP_LOGW(TAG, "Factory reset triggered via GPIO%d", gpio);
    nvs_flash_erase();
    pm_restart();
}
#endif

void app_main(void)
{
    /* Initialize status LED first for visual feedback */
    status_init();
    status_set_busy(true);

    device_name_get(g_device_name, sizeof(g_device_name));
    ESP_LOGI(TAG, "Thread End Device - %s", g_device_name);

    /* Power management */
    pm_config_t pm_cfg = {
        .light_sleep_enable = false,
        .stats_interval_ms = PM_STATS_INTERVAL_MS,
#if CONFIG_FACTORY_RESET_BUTTON_ENABLED
        .wake_gpios = { { .gpio = CONFIG_FACTORY_RESET_BUTTON_GPIO, .active_low = true } },
        .num_wake_gpios = 1,
        .wake_cb = on_factory_reset_button,
#endif
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
    ESP_ERROR_CHECK(thread_comms_init(g_device_name, THREAD_COMMS_SOURCE_END_DEVICE));

    status_it_worked();
    status_set_busy(false);

    /* Initialize sensors and relay */
    sensors_t *sensors = sensors_init();
    g_relay = relay_init();

    ESP_LOGI(TAG, "Entering main loop (interval: %d ms)", CONFIG_MAIN_LOOP_INTERVAL_MS);

    /* Main loop */
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        /* Read sensors */
        sensors_read(sensors);

        /* Get current values */
        const float *temp = sensors_get_temperature(sensors);
        const float *hum = sensors_get_humidity(sensors);
        const bool *relay_state = relay_get_state(g_relay);

        /* Build and send report */
        thread_comms_report_t report = {0};
        strncpy(report.device_id, g_device_name, sizeof(report.device_id) - 1);
        if (temp) {
            report.has_temperature = true;
            report.temperature = *temp;
        }
        if (hum) {
            report.has_humidity = true;
            report.humidity = *hum;
        }
        if (relay_state) {
            report.has_relay_state = true;
            report.relay_state = *relay_state;
        }

        esp_err_t err = thread_comms_send_report(&report);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Sent report: temp=%.1f humidity=%.1f%% relay=%s",
                     temp ? *temp : 0, hum ? *hum : 0,
                     relay_state ? (*relay_state ? "ON" : "OFF") : "N/A");
        } else {
            ESP_LOGW(TAG, "Failed to send report: %s", esp_err_to_name(err));
        }

        /* Poll parent for buffered messages */
        thread_comms_poll();

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(CONFIG_MAIN_LOOP_INTERVAL_MS));
    }
}
