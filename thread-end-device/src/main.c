#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sleep.h"
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

/* RTC memory survives deep sleep */
static RTC_DATA_ATTR bool g_relay_state = false;

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
        g_relay_state = msg->relay_cmd.relay_state;  /* Save to RTC memory */
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

    /* Power management - DFS only, light sleep breaks Thread messaging */
    pm_config_t pm_cfg = {
        .stats_interval_ms = PM_STATS_INTERVAL_MS,
        .light_sleep_enable = false,
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

    thread_comms_config_t comms_cfg = {
        .device_id = g_device_name,
        .source = THREAD_COMMS_SOURCE_END_DEVICE,
        .use_uart_rcp = false,  /* End device always uses native radio */
    };
    ESP_ERROR_CHECK(thread_comms_init(&comms_cfg));

    status_it_worked();
    status_set_busy(false);

    /* Initialize sensors and relay */
    sensors_t *sensors = sensors_init();
    g_relay = relay_init(g_relay_state);

    /* Duty cycle: active for ACTIVE_MS, then deep sleep for SLEEP_MS */
    #define ACTIVE_MS  3000
    #define SLEEP_MS   15000
    #define LOOP_MS    500   /* Poll interval during active period */

    ESP_LOGI(TAG, "Duty cycle: %dms active, %dms sleep", ACTIVE_MS, SLEEP_MS);

    /* Active period - can send reports and receive commands */
    TickType_t active_start = xTaskGetTickCount();
    bool report_sent = false;

    while ((xTaskGetTickCount() - active_start) < pdMS_TO_TICKS(ACTIVE_MS)) {
        /* Send report once per active period */
        if (!report_sent) {
            sensors_read(sensors);

            const float *temp = sensors_get_temperature(sensors);
            const float *hum = sensors_get_humidity(sensors);
            const bool *relay_state = relay_get_state(g_relay);

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
                report_sent = true;
            } else {
                ESP_LOGW(TAG, "Failed to send report: %s", esp_err_to_name(err));
            }
        }

        /* Stay active to receive commands */
        vTaskDelay(pdMS_TO_TICKS(LOOP_MS));
    }

    /* Shutdown Thread gracefully */
    ESP_LOGI(TAG, "Active period ended, entering deep sleep...");
    thread_comms_deinit();

    /* Enter deep sleep */
    pm_deep_sleep_for(SLEEP_MS);
    /* Never reached - device resets on wake */
}
