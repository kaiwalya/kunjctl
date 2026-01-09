#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_types.h"
#include "esp_vfs_eventfd.h"
#include "nvs_flash.h"

#include "openthread/dataset.h"
#include "openthread/instance.h"
#include "openthread/ip6.h"
#include "openthread/link.h"
#include "openthread/logging.h"
#include "openthread/thread.h"

#include "device_name.h"
#include "power_management.h"
#include "sensors.h"
#include "status.h"
#include "relay.h"

static const char *TAG = "thread-end-device";

#define PM_STATS_INTERVAL_MS 60000

static const char *role_to_string(otDeviceRole role)
{
    switch (role) {
        case OT_DEVICE_ROLE_DISABLED: return "Disabled";
        case OT_DEVICE_ROLE_DETACHED: return "Detached";
        case OT_DEVICE_ROLE_CHILD:    return "Child";
        case OT_DEVICE_ROLE_ROUTER:   return "Router";
        case OT_DEVICE_ROLE_LEADER:   return "Leader";
        default:                      return "Unknown";
    }
}

static void configure_sed_mode(otInstance *instance)
{
    /* Configure as Sleepy End Device */
    otLinkModeConfig mode = {0};
    mode.mRxOnWhenIdle = false;  /* Sleep between polls */
    mode.mDeviceType = false;    /* MTD (not FTD) */
    mode.mNetworkData = false;   /* Minimal network data */
    otThreadSetLinkMode(instance, mode);

    /* Disable automatic polling - we'll manually poll in main loop */
    otLinkSetPollPeriod(instance, 0);

    ESP_LOGI(TAG, "Configured as Sleepy End Device (manual polling)");
}

static void ot_state_changed(otChangedFlags flags, void *ctx)
{
    if (flags & OT_CHANGED_THREAD_ROLE) {
        otInstance *instance = esp_openthread_get_instance();
        otDeviceRole role = otThreadGetDeviceRole(instance);
        ESP_LOGI(TAG, "Role changed: %s", role_to_string(role));
    }
}

static void ot_mainloop(void *arg)
{
    esp_openthread_launch_mainloop();
    vTaskDelete(NULL);
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

    char device_name[32];
    device_name_get(device_name, sizeof(device_name));
    ESP_LOGI(TAG, "Thread End Device - %s", device_name);

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

    /* Initialize netif and event loop */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* OpenThread */
    esp_vfs_eventfd_config_t eventfd_config = { .max_fds = 3 };
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_config));

    esp_openthread_platform_config_t ot_config = {
        .radio_config = { .radio_mode = RADIO_MODE_NATIVE },
        .host_config = { .host_connection_mode = HOST_CONNECTION_MODE_NONE },
        .port_config = { .storage_partition_name = "nvs", .netif_queue_size = 10, .task_queue_size = 10 },
    };
    ESP_ERROR_CHECK(esp_openthread_init(&ot_config));

    /* Create and attach netif with glue - enables IPv6 packet rx/tx */
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_OPENTHREAD();
    esp_netif_t *netif = esp_netif_new(&netif_cfg);
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_openthread_netif_glue_init(&ot_config)));

    otLoggingSetLevel(OT_LOG_LEVEL_NOTE);

    otInstance *instance = esp_openthread_get_instance();

    /* Set up network dataset - must match router's credentials */
    otOperationalDataset dataset;
    memset(&dataset, 0, sizeof(dataset));

    /* Network name */
    memcpy(dataset.mNetworkName.m8, "HomeAuto", 9);
    dataset.mComponents.mIsNetworkNamePresent = true;

    /* Channel */
    dataset.mChannel = 15;
    dataset.mComponents.mIsChannelPresent = true;

    /* PAN ID */
    dataset.mPanId = 0x1234;
    dataset.mComponents.mIsPanIdPresent = true;

    /* Extended PAN ID (8 bytes) */
    uint8_t ext_pan_id[8] = {0x11, 0x11, 0x11, 0x11, 0x22, 0x22, 0x22, 0x22};
    memcpy(dataset.mExtendedPanId.m8, ext_pan_id, 8);
    dataset.mComponents.mIsExtendedPanIdPresent = true;

    /* Network key (16 bytes) - MUST match router */
    uint8_t network_key[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                               0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    memcpy(dataset.mNetworkKey.m8, network_key, 16);
    dataset.mComponents.mIsNetworkKeyPresent = true;

    /* Mesh-local prefix */
    uint8_t mesh_local_prefix[8] = {0xfd, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    memcpy(dataset.mMeshLocalPrefix.m8, mesh_local_prefix, 8);
    dataset.mComponents.mIsMeshLocalPrefixPresent = true;

    esp_openthread_lock_acquire(portMAX_DELAY);
    otDatasetSetActive(instance, &dataset);
    otSetStateChangedCallback(instance, ot_state_changed, NULL);
    otIp6SetEnabled(instance, true);
    otThreadSetEnabled(instance, true);
    esp_openthread_lock_release();

    xTaskCreate(ot_mainloop, "ot_mainloop", 4096, NULL, 5, NULL);

    /* Wait for network join or timeout */
#if CONFIG_JOIN_TIMEOUT_MS > 0
    ESP_LOGI(TAG, "Waiting up to %d ms to join network...", CONFIG_JOIN_TIMEOUT_MS);
    bool joined = false;
    for (int elapsed = 0; elapsed < CONFIG_JOIN_TIMEOUT_MS; elapsed += 500) {
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_openthread_lock_acquire(portMAX_DELAY);
        otDeviceRole role = otThreadGetDeviceRole(instance);
        esp_openthread_lock_release();
        if (role >= OT_DEVICE_ROLE_CHILD) {
            joined = true;
            break;
        }
    }

    if (!joined) {
        ESP_LOGW(TAG, "Failed to join network after %d ms, entering deep sleep", CONFIG_JOIN_TIMEOUT_MS);
        status_set_busy(false);
        pm_deep_sleep();
        /* Never reached */
    }
#endif

    ESP_LOGI(TAG, "Successfully joined Thread network!");
    status_it_worked();
    status_set_busy(false);

    esp_openthread_lock_acquire(portMAX_DELAY);
    configure_sed_mode(instance);
    esp_openthread_lock_release();

    /* Initialize sensors and relay */
    sensors_t *sensors = sensors_init();
    relay_t *relay = relay_init();

    ESP_LOGI(TAG, "Entering main loop (interval: %d ms)", CONFIG_MAIN_LOOP_INTERVAL_MS);

    /* Main loop */
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        /* Read sensors */
        sensors_read(sensors);

        /* Log sensor values */
        const float *temp = sensors_get_temperature(sensors);
        const float *hum = sensors_get_humidity(sensors);
        const bool *relay_state = relay_get_state(relay);

        if (temp && hum) {
            ESP_LOGI(TAG, "Sensors: temp=%.1f°C humidity=%.1f%%", *temp, *hum);
        }
        if (relay_state) {
            ESP_LOGI(TAG, "Relay: %s", *relay_state ? "ON" : "OFF");
        }

        /* TODO: Queue UDP packet with sensor data here */

        /* Poll parent - sends queued data and receives buffered commands */
        esp_openthread_lock_acquire(portMAX_DELAY);
        otLinkSendDataRequest(instance);
        esp_openthread_lock_release();

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(CONFIG_MAIN_LOOP_INTERVAL_MS));
    }
}
