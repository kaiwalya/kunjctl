#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "power_management.h"
#include "status.h"
#include "sensors.h"
#include "device_name.h"
#include "comms.h"

#include "state/state.h"

static const char *TAG = "node";

#define MAIN_LOOP_INTERVAL_MS   10000
#define PM_STATS_INTERVAL_MS    60000

void app_main(void)
{
    /* Initialize status LED */
    status_init();
    status_set_busy(true);

    char device_name[32];
    device_name_get(device_name, sizeof(device_name));
    ESP_LOGI(TAG, "Booting %s", device_name);
    
    /* Initialize power management */
    pm_config_t pm_cfg = {
        .num_wake_gpios = 0,
        .light_sleep_enable = true,
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
    if (comms_init(device_name) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init comms");
        return;
    }

    /* Initialize state */
    state_t *state = state_init();
    if (!state) {
        ESP_LOGE(TAG, "Failed to initialize state");
        return;
    }

    const pairing_state_t pairing_state = state_get_pairing(state);
    if (pairing_state == PAIRING_STATE_UNPAIRED) {
        ESP_LOGW(TAG, "Device is unpaired, only sensor data will be broadcasted");
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
        comms_send_hello();
        // TODO: comms_send_sensor_data(temp, hum);
        // TODO: comms_wait_for_input();
        comms_close();

        status_set_busy(false);

        /* Sleep until next interval (accounts for time spent working) */
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(MAIN_LOOP_INTERVAL_MS));
    }
}
