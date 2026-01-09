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
#include "openthread/logging.h"
#include "openthread/thread.h"

#include "device_name.h"
#include "power_management.h"

static const char *TAG = "thread-router";

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

static void ot_state_changed(otChangedFlags flags, void *ctx)
{
    otInstance *instance = esp_openthread_get_instance();

    if (flags & OT_CHANGED_THREAD_ROLE) {
        otDeviceRole role = otThreadGetDeviceRole(instance);
        ESP_LOGI(TAG, "Role changed: %s", role_to_string(role));
    }

    if (flags & OT_CHANGED_THREAD_CHILD_ADDED) {
        ESP_LOGI(TAG, "Child joined the network!");
    }

    if (flags & OT_CHANGED_THREAD_CHILD_REMOVED) {
        ESP_LOGI(TAG, "Child left the network");
    }
}

static void ot_mainloop(void *arg)
{
    esp_openthread_launch_mainloop();
    vTaskDelete(NULL);
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

    /* Initialize netif and event loop */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* OpenThread platform init */
    esp_vfs_eventfd_config_t eventfd_config = { .max_fds = 3 };
    esp_vfs_eventfd_register(&eventfd_config);

    esp_openthread_platform_config_t ot_config = {
        .radio_config = { .radio_mode = RADIO_MODE_NATIVE },
        .host_config = { .host_connection_mode = HOST_CONNECTION_MODE_NONE },
        .port_config = { .storage_partition_name = "nvs", .netif_queue_size = 10, .task_queue_size = 10 },
    };
    ESP_ERROR_CHECK(esp_openthread_init(&ot_config));

    /* Create OpenThread netif */
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_OPENTHREAD();
    esp_netif_t *netif = esp_netif_new(&netif_cfg);
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_openthread_netif_glue_init(&ot_config)));

    otLoggingSetLevel(OT_LOG_LEVEL_NOTE);

    otInstance *instance = esp_openthread_get_instance();

    /* Set up network dataset */
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

    /* Network key (16 bytes) */
    uint8_t network_key[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                               0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    memcpy(dataset.mNetworkKey.m8, network_key, 16);
    dataset.mComponents.mIsNetworkKeyPresent = true;

    /* Mesh-local prefix */
    uint8_t mesh_local_prefix[8] = {0xfd, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    memcpy(dataset.mMeshLocalPrefix.m8, mesh_local_prefix, 8);
    dataset.mComponents.mIsMeshLocalPrefixPresent = true;

    /* Active timestamp (required to form network) */
    dataset.mActiveTimestamp.mSeconds = 1;
    dataset.mActiveTimestamp.mTicks = 0;
    dataset.mActiveTimestamp.mAuthoritative = false;
    dataset.mComponents.mIsActiveTimestampPresent = true;

    /* Security policy (allow joining) */
    dataset.mSecurityPolicy.mRotationTime = 672;  /* Key rotation in hours */
    dataset.mSecurityPolicy.mObtainNetworkKeyEnabled = true;
    dataset.mSecurityPolicy.mNativeCommissioningEnabled = true;
    dataset.mSecurityPolicy.mRoutersEnabled = true;
    dataset.mSecurityPolicy.mExternalCommissioningEnabled = true;
    dataset.mComponents.mIsSecurityPolicyPresent = true;

    /* PSKc - Pre-Shared Key for Commissioner (16 bytes) */
    uint8_t pskc[16] = {0x3a, 0xa5, 0x5f, 0x91, 0xca, 0x47, 0xd1, 0xe4,
                        0xe7, 0x1a, 0x08, 0xcb, 0x35, 0xe9, 0x15, 0x91};
    memcpy(dataset.mPskc.m8, pskc, 16);
    dataset.mComponents.mIsPskcPresent = true;

    esp_openthread_lock_acquire(portMAX_DELAY);
    otDatasetSetActive(instance, &dataset);
    otSetStateChangedCallback(instance, ot_state_changed, NULL);
    otIp6SetEnabled(instance, true);
    otThreadSetEnabled(instance, true);
    esp_openthread_lock_release();

    xTaskCreate(ot_mainloop, "ot_mainloop", 4096, NULL, 5, NULL);

    /* Debug: print active dataset periodically */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        otOperationalDataset active;
        esp_openthread_lock_acquire(portMAX_DELAY);
        otDatasetGetActive(instance, &active);
        esp_openthread_lock_release();
        ESP_LOGI(TAG, "Network: '%s', ExtPAN: %02x%02x%02x%02x%02x%02x%02x%02x",
                 active.mNetworkName.m8,
                 active.mExtendedPanId.m8[0], active.mExtendedPanId.m8[1],
                 active.mExtendedPanId.m8[2], active.mExtendedPanId.m8[3],
                 active.mExtendedPanId.m8[4], active.mExtendedPanId.m8[5],
                 active.mExtendedPanId.m8[6], active.mExtendedPanId.m8[7]);
    }
}
