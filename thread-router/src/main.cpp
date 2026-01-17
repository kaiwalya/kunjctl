#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
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

#include "bridge_nvs.hpp"

using namespace esp_matter;

static const char *TAG = "tr-router";

#define PM_STATS_INTERVAL_MS 60000

// Mutex for serializing access to bridge_nvs
static SemaphoreHandle_t s_bridge_mutex = nullptr;

// RAII lock guard
class BridgeLock {
public:
    BridgeLock() { if (s_bridge_mutex) xSemaphoreTake(s_bridge_mutex, portMAX_DELAY); }
    ~BridgeLock() { if (s_bridge_mutex) xSemaphoreGive(s_bridge_mutex); }
    BridgeLock(const BridgeLock&) = delete;
    BridgeLock& operator=(const BridgeLock&) = delete;
};

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
    if (msg->type != THREAD_COMMS_MSG_REPORT) {
        return;
    }

    const thread_comms_report_t *r = &msg->report;
    ESP_LOGI(TAG, "Report from '%s': temp=%.1f humidity=%.1f%% relay=%s",
             r->device_id,
             r->has_temperature ? r->temperature : 0,
             r->has_humidity ? r->humidity : 0,
             r->has_relay_state ? (r->relay_state ? "ON" : "OFF") : "N/A");

    // Get hex suffix for NVS lookup
    const char *hex = bridge_nvs_get_hex_suffix(r->device_id);
    if (!hex) {
        ESP_LOGW(TAG, "Invalid device_id format: %s", r->device_id);
        return;
    }

    BridgeLock lock;

    // Try to load existing device
    auto existing = bridge_nvs_load_device(hex);

    BridgeDeviceState device;
    if (existing.has_value()) {
        device = existing.value();
        ESP_LOGI(TAG, "Found existing device: %s (endpoint %u)", device.device_id.c_str(), device.endpoint_id);
    } else {
        // New device - allocate endpoint ID
        device.device_id = r->device_id;
        device.endpoint_id = bridge_nvs_alloc_endpoint_id();
        ESP_LOGI(TAG, "New device registered: %s (endpoint %u)", device.device_id.c_str(), device.endpoint_id);
    }

    // Update sensor state from report
    if (r->has_temperature) {
        device.temperature = r->temperature;
    }
    if (r->has_humidity) {
        device.humidity = r->humidity;
    }
    if (r->has_relay_state) {
        device.relay_state = r->relay_state;
    }

    // Save to NVS
    esp_err_t err = bridge_nvs_save_device(device);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save device: %s", esp_err_to_name(err));
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

    /* Bridge state mutex and NVS */
    s_bridge_mutex = xSemaphoreCreateMutex();
    if (!s_bridge_mutex) {
        ESP_LOGE(TAG, "Failed to create bridge mutex");
        return;
    }
    ESP_ERROR_CHECK(bridge_nvs_init());

    /* Log existing devices from NVS */
    {
        BridgeLock lock;
        auto devices = bridge_nvs_load_all_devices();
        ESP_LOGI(TAG, "Loaded %zu devices from NVS", devices.size());
        for (const auto &dev : devices) {
            ESP_LOGI(TAG, "  - %s (endpoint %u)", dev.device_id.c_str(), dev.endpoint_id);
        }
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

    /* Create Matter node */
    ESP_LOGI(TAG, "Creating Matter node...");
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) {
        ESP_LOGE(TAG, "Failed to create Matter node");
        return;
    }

    /* Create aggregator endpoint for bridging Thread devices */
    endpoint::aggregator::config_t aggregator_config;
    endpoint_t *aggregator = endpoint::aggregator::create(node, &aggregator_config, ENDPOINT_FLAG_NONE, NULL);
    if (!aggregator) {
        ESP_LOGE(TAG, "Failed to create aggregator endpoint");
        return;
    }
    ESP_LOGI(TAG, "Matter bridge created (aggregator endpoint ready)");

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
