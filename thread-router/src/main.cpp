#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
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

// Boot button GPIO (active low)
#define BOOT_BUTTON_GPIO GPIO_NUM_0
#define BOOT_BUTTON_HOLD_MS 3000
#define BOOT_BUTTON_CHECK_DELAY_MS 1000

#include <esp_matter.h>
#include <esp_matter_core.h>
#include <esp_matter_endpoint.h>
#include <app/clusters/on-off-server/on-off-server.h>

#include "bridge_state.hpp"

using namespace esp_matter;

static const char *TAG = "tr-router";

#define PM_STATS_INTERVAL_MS 60000

// Mutex for serializing access to bridge state
static SemaphoreHandle_t s_bridge_mutex = nullptr;

// Global bridge state manager
static BridgeState g_bridge;

// RAII lock guard (recursive mutex to allow nested locking)
class BridgeLock {
public:
    BridgeLock() { if (s_bridge_mutex) xSemaphoreTakeRecursive(s_bridge_mutex, portMAX_DELAY); }
    ~BridgeLock() { if (s_bridge_mutex) xSemaphoreGiveRecursive(s_bridge_mutex); }
    BridgeLock(const BridgeLock&) = delete;
    BridgeLock& operator=(const BridgeLock&) = delete;
};

// Matter attribute update callback
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type,
                                         uint16_t endpoint_id, uint32_t cluster_id,
                                         uint32_t attribute_id, esp_matter_attr_val_t *val,
                                         void *priv_data)
{
    // Handle OnOff cluster commands from Matter controllers
    // Skip if this update is from our own Thread report processing
    if (type == attribute::PRE_UPDATE &&
        cluster_id == chip::app::Clusters::OnOff::Id &&
        attribute_id == chip::app::Clusters::OnOff::Attributes::OnOff::Id &&
        !g_bridge.updating_from_thread) {
        BridgeLock lock;
        g_bridge.queue_cmd(endpoint_id, val->val.b);
    }

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

// Boot button task - monitors for factory reset gesture
// Hold 3s = erase bridge data, hold 6s = full factory reset
static void boot_button_task(void *arg)
{
    // Configure GPIO
    gpio_set_direction(BOOT_BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOOT_BUTTON_GPIO, GPIO_PULLUP_ONLY);

    ESP_LOGI(TAG, "Boot button task started (GPIO%d)", BOOT_BUTTON_GPIO);

    for (;;) {
        // Wait for button press
        if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            ESP_LOGW(TAG, "Boot button detected - hold 3s for bridge reset, 6s for factory reset...");
            int held_ms = 0;
            bool bridge_reset_logged = false;

            while (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                vTaskDelay(pdMS_TO_TICKS(100));
                held_ms += 100;

                if (held_ms >= BOOT_BUTTON_HOLD_MS && !bridge_reset_logged) {
                    ESP_LOGW(TAG, "3s - release now for bridge reset, keep holding for factory reset...");
                    bridge_reset_logged = true;
                }

                if (held_ms >= BOOT_BUTTON_HOLD_MS * 2) {
                    // 6 seconds - full factory reset
                    ESP_LOGW(TAG, "Factory reset - erasing all NVS...");
                    nvs_flash_erase();
                    ESP_LOGW(TAG, "All NVS erased. Restarting...");
                    vTaskDelay(pdMS_TO_TICKS(500));
                    esp_restart();
                }
            }

            // Button released - check what action to take
            if (held_ms >= BOOT_BUTTON_HOLD_MS) {
                ESP_LOGW(TAG, "Erasing bridge device data...");
                bridge_nvs_erase_all();
                ESP_LOGW(TAG, "Bridge data erased. Restarting...");
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            } else {
                ESP_LOGI(TAG, "Button released - cancelled");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));  // Poll interval
    }
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

    BridgeLock lock;
    g_bridge.on_report(r);
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
    s_bridge_mutex = xSemaphoreCreateRecursiveMutex();
    if (!s_bridge_mutex) {
        ESP_LOGE(TAG, "Failed to create bridge mutex");
        return;
    }
    ESP_ERROR_CHECK(bridge_nvs_init());

    /* Start boot button monitor task (checks for factory reset gesture) */
    xTaskCreate(boot_button_task, "boot_btn", 2048, NULL, 5, NULL);

    /* ESP-IDF networking stack */
    esp_vfs_eventfd_config_t eventfd_config = { .max_fds = 3 };
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_config));
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Silence verbose Matter logs (before Matter starts) */
    esp_log_level_set("chip", ESP_LOG_WARN);
    esp_log_level_set("chip[IM]", ESP_LOG_WARN);
    esp_log_level_set("chip[EM]", ESP_LOG_WARN);
    esp_log_level_set("chip[DMG]", ESP_LOG_WARN);
    esp_log_level_set("chip[DIS]", ESP_LOG_WARN);
    esp_log_level_set("chip[DL]", ESP_LOG_WARN);
    esp_log_level_set("chip[SVR]", ESP_LOG_WARN);

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

    /* Initialize bridge state (after Matter starts) */
    {
        BridgeLock lock;
        uint16_t aggregator_id = endpoint::get_id(aggregator);
        err = g_bridge.init(node, aggregator_id);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize bridge state: %s", esp_err_to_name(err));
            return;
        }
    }
    ESP_LOGI(TAG, "Bridge state initialized");

    /* Thread networking and comms (after bridge is ready to receive callbacks) */
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
    ESP_LOGI(TAG, "Thread comms initialized - ready for devices!");
}
