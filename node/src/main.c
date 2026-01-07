#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "power_management.h"
#include "device_name.h"
#include "comms.h"
#include "status.h"
#include "sensors.h"
#include "state/state.h"

static const char *TAG = "node";

#define MAIN_LOOP_INTERVAL_MS       10000
#define PM_STATS_INTERVAL_MS        60000
#define UNPAIRED_ADV_DURATION_MS    2000   /* Broadcast Hello for 2 seconds */
#define UNPAIRED_SCAN_DURATION_MS   8000   /* Listen for hub Hello for 8 seconds */

/*── Unpaired Mode ──*/

static volatile bool g_hub_seen = false;

static void on_hello_received(const comms_hello_t *hello) {
    if (hello->source == COMMS_SOURCE_HUB) {
        ESP_LOGI(TAG, "Hub discovered: %s", hello->device_id);
        g_hub_seen = true;
    }
}

static void app_main_unpaired(state_t *state) __attribute__((noreturn));

static void app_main_unpaired(state_t *state) {
    ESP_LOGW(TAG, "Device unpaired - broadcast + scan cycle");

    g_hub_seen = false;

    /* Phase 1: Broadcast Hello for 2 seconds */
    ESP_LOGI(TAG, "Broadcasting Hello...");
    comms_open();
    comms_send_hello_for(UNPAIRED_ADV_DURATION_MS);

    /* Phase 2: Scan for hub Hello for 8 seconds */
    ESP_LOGI(TAG, "Scanning for hub...");
    comms_start_scanning(on_hello_received);
    vTaskDelay(pdMS_TO_TICKS(UNPAIRED_SCAN_DURATION_MS));
    comms_stop_scanning();
    comms_close();

    /* Check if we discovered a hub */
    if (g_hub_seen) {
        ESP_LOGI(TAG, "Hub found! Marking as paired.");
        state_set_pairing(state, PAIRING_STATE_PAIRED);
        status_set_busy(false);
        esp_restart();
        /* Never reached */
    }

    ESP_LOGI(TAG, "No hub found, entering deep sleep");
    status_set_busy(false);
    pm_deep_sleep();
    /* Never reached */
}

void app_main(void)
{
    /* Initialize status LED */
    status_init();
    status_set_busy(true);

    char device_name[32];
    device_name_get(device_name, sizeof(device_name));
    ESP_LOGI(TAG, "Booting node %s", device_name);
    
    /* Initialize power management */
    pm_config_t pm_cfg = {
        .num_wake_gpios = 0,
        .light_sleep_enable = false,  // TODO: enable for production
        .stats_interval_ms = PM_STATS_INTERVAL_MS,
    };
    pm_init(&pm_cfg);
    
    /* Initialize NVS (required for BLE and state) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* Initialize comms */
    if (comms_init(device_name, COMMS_SOURCE_NODE) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init comms");
        return;
    }

    /* Initialize state */
    state_t *state = state_init();
    if (!state) {
        ESP_LOGE(TAG, "Failed to initialize state");
        return;
    }

    /* Branch based on pairing state */
    if (state_get_pairing(state) == PAIRING_STATE_UNPAIRED) {
        app_main_unpaired(state);
        /* Never returns */
    }
    
    /* Initialize sensors */
    sensors_t *sensors = sensors_init();
    if (!sensors) {
        ESP_LOGE(TAG, "Failed to initialize sensors");
        return;
    }

    

    ESP_LOGI(TAG, "Starting main loop (every %d seconds)",
             MAIN_LOOP_INTERVAL_MS / 1000);

    status_set_busy(false);

    TickType_t last_wake = xTaskGetTickCount();

    /* Main loop */
    for (;;) {
        status_set_busy(true);

        /* Read sensors */
        sensors_read(sensors);
        const float *temp = sensors_get_temperature(sensors);
        const float *hum = sensors_get_humidity(sensors);
        if (temp) ESP_LOGI(TAG, "Temperature: %.1f C", *temp);
        if (hum)  ESP_LOGI(TAG, "Humidity: %.1f %%", *hum);

        /* Communicate */
        comms_open();
        comms_send_hello();  /* Blocks until advertising complete */
        // TODO: comms_send_sensor_data(temp, hum);
        // TODO: comms_wait_for_input();
        comms_close();

        status_set_busy(false);

        /* Sleep until next interval (accounts for time spent working) */
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(MAIN_LOOP_INTERVAL_MS));
    }
}
