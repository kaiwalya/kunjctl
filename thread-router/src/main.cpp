#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_vfs_eventfd.h"
#include "nvs_flash.h"

extern "C" {
#include "device_name.h"
#include "power_management.h"
#include "thread_comms.h"
}

#include <esp_matter.h>
#include <esp_matter_core.h>
#include <esp_matter_endpoint.h>

using namespace esp_matter;

static const char *TAG = "thread-router";

#define PM_STATS_INTERVAL_MS 60000

// Matter attribute update callback
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type,
                                         uint16_t endpoint_id, uint32_t cluster_id,
                                         uint32_t attribute_id, esp_matter_attr_val_t *val,
                                         void *priv_data)
{
    ESP_LOGI(TAG, "Attribute update: endpoint=%d, cluster=0x%lx, attr=0x%lx",
             endpoint_id, (unsigned long)cluster_id, (unsigned long)attribute_id);
    return ESP_OK;
}

// Matter identification callback
static esp_err_t app_identification_cb(identification::callback_type_t type,
                                       uint16_t endpoint_id,
                                       uint8_t effect_id, uint8_t effect_variant,
                                       void *priv_data)
{
    ESP_LOGI(TAG, "Identification: endpoint=%d, effect=%d", endpoint_id, effect_id);
    return ESP_OK;
}

// Thread message callback
static void on_thread_message(const thread_comms_message_t *msg)
{
    if (msg->type == THREAD_COMMS_MSG_REPORT) {
        const thread_comms_report_t *r = &msg->report;
        ESP_LOGI(TAG, "Report from '%s': temp=%.1f humidity=%.1f%% relay=%s",
                 r->device_id,
                 r->has_temperature ? r->temperature : 0,
                 r->has_humidity ? r->humidity : 0,
                 r->has_relay_state ? (r->relay_state ? "ON" : "OFF") : "N/A");

        if (r->has_relay_state) {
            thread_comms_relay_cmd_t cmd = {};
            strncpy(cmd.device_id, r->device_id, sizeof(cmd.device_id) - 1);
            cmd.relay_state = !r->relay_state;

            ESP_LOGI(TAG, "Sending relay command to '%s': %s",
                     cmd.device_id, cmd.relay_state ? "ON" : "OFF");
            thread_comms_send_relay_cmd(&cmd);
        }
    }
}

extern "C" void app_main(void)
{
    char device_name[32];
    device_name_get(device_name, sizeof(device_name));
    ESP_LOGI(TAG, "Thread Router - %s", device_name);

    /* Power management */
    pm_config_t pm_cfg = {
        .stats_interval_ms = PM_STATS_INTERVAL_MS,
        .light_sleep_enable = false,  /* Router must stay awake */
    };
    pm_init(&pm_cfg);

    /* NVS */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase");
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
        .device_id = device_name,
        .source = THREAD_COMMS_SOURCE_ROUTER,
#if CONFIG_OPENTHREAD_RADIO_SPINEL_UART
        .use_uart_rcp = true,
        .uart = {
            .port = 1,
            .tx_pin = 18,
            .rx_pin = 17,
        },
#else
        .use_uart_rcp = false,
#endif
    };
    ESP_ERROR_CHECK(thread_comms_init(&comms_cfg));

    /* Create Matter node (empty - just root device) */
    ESP_LOGI(TAG, "Creating Matter node...");
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }
    ESP_LOGI(TAG, "Matter node created");

    /* Start Matter */
    err = esp_matter::start(NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Matter: %d", err);
        return;
    }
    ESP_LOGI(TAG, "Matter started - ready for commissioning!");

    /* Keep running */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
