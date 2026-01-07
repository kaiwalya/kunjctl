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
#define PAIRING_DELAY_MS            2000
#define PAIRING_BROADCAST_MS        2000
#define PAIRING_TASK_STACK_SIZE     4096
#define PAIRING_TASK_NAME_PREFIX    "hi-"

/* Forward declaration */
static void on_hello_received(const comms_hello_t *hello);

/*── Pairing Response Task ──*/

static void pairing_task(void *arg) {
    char *device_id = (char *)arg;

    ESP_LOGI(TAG, "Pairing task started for %s", device_id);

    /* Wait before responding */
    vTaskDelay(pdMS_TO_TICKS(PAIRING_DELAY_MS));

    /* Stop scanning, broadcast Hello, resume scanning */
    comms_stop_scanning();
    comms_send_hello_for(PAIRING_BROADCAST_MS);
    comms_start_scanning(on_hello_received);

    ESP_LOGI(TAG, "Pairing response sent for %s", device_id);

    free(device_id);
    vTaskDelete(NULL);
}

static void on_hello_received(const comms_hello_t *hello) {
    if (hello->source != COMMS_SOURCE_NODE) {
        return;
    }

    ESP_LOGI(TAG, "Received Hello from node: %s", hello->device_id);

    /* Build task name: "hi-<device_id>" (truncated to fit) */
    char task_name[configMAX_TASK_NAME_LEN];
    int max_id_len = configMAX_TASK_NAME_LEN - sizeof(PAIRING_TASK_NAME_PREFIX);
    snprintf(task_name, sizeof(task_name), "%s%.*s",
             PAIRING_TASK_NAME_PREFIX, max_id_len, hello->device_id);

    /* Check if pairing task already exists */
    if (xTaskGetHandle(task_name) != NULL) {
        ESP_LOGD(TAG, "Pairing task already active for %s", hello->device_id);
        return;
    }

    /* Create pairing task (pass copy of device_id) */
    char *device_id_copy = strdup(hello->device_id);
    if (!device_id_copy) {
        ESP_LOGE(TAG, "Failed to allocate device_id");
        return;
    }

    BaseType_t ret = xTaskCreate(pairing_task, task_name,
                                  PAIRING_TASK_STACK_SIZE, device_id_copy,
                                  tskIDLE_PRIORITY + 1, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create pairing task");
        free(device_id_copy);
    }
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

    /* Open radio and start scanning */
    if (comms_open() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open comms");
        return;
    }

    if (comms_start_scanning(on_hello_received) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start scanning");
        return;
    }

    ESP_LOGI(TAG, "Hub ready, scanning for nodes...");

    /* Main loop (hub stays awake, scanning continuously) */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
