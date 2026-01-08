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
#include "relay.h"
#include "state/state.h"

static const char *TAG = "node";

#define MAIN_LOOP_INTERVAL_MS       10000
#define PM_STATS_INTERVAL_MS        60000
#define UNPAIRED_ADV_DURATION_MS    2000   /* Broadcast Hello for 2 seconds */
#define UNPAIRED_SCAN_DURATION_MS   8000   /* Listen for hub Hello for 8 seconds */
#define REPORT_DURATION_MS          500    /* Broadcast report for 500ms */
#define COMMAND_SCAN_DURATION_MS    3000   /* Listen for commands for 3 seconds */

/*── Factory Reset ──*/

#if CONFIG_FACTORY_RESET_BUTTON_ENABLED
static void on_factory_reset_button(gpio_num_t gpio) {
    ESP_LOGW(TAG, "Factory reset triggered via GPIO%d", gpio);
    nvs_flash_erase();
    pm_restart();
}
#endif

/*── Unpaired Mode ──*/

static void app_main_unpaired(state_t *state) __attribute__((noreturn));

static void app_main_unpaired(state_t *state) {
    ESP_LOGW(TAG, "Device unpaired - broadcast + scan cycle");
    status_set_busy(true);  /* Red LED while unpaired */

    comms_open();

    /* Phase 1: Broadcast Hello */
    ESP_LOGI(TAG, "Broadcasting Hello...");
    comms_send_hello_for(UNPAIRED_ADV_DURATION_MS);

    /* Phase 2: Scan for hub Hello */
    ESP_LOGI(TAG, "Scanning for hub...");
    comms_message_t messages[4];
    int count = comms_scan_for(UNPAIRED_SCAN_DURATION_MS, messages, 4);

    comms_close();

    /* Check if we discovered a hub */
    for (int i = 0; i < count; i++) {
        if (messages[i].has_hello && messages[i].hello.source == COMMS_SOURCE_HUB) {
            ESP_LOGI(TAG, "Hub found: %s! Marking as paired.", messages[i].device_id);
            state_set_pairing(state, PAIRING_STATE_PAIRED);
            status_it_worked();
            pm_restart();
            /* Never reached */
        }
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
        .light_sleep_enable = false,  // TODO: enable for production
        .stats_interval_ms = PM_STATS_INTERVAL_MS,
#if CONFIG_FACTORY_RESET_BUTTON_ENABLED
        .wake_gpios = { { .gpio = CONFIG_FACTORY_RESET_BUTTON_GPIO, .active_low = true } },
        .num_wake_gpios = 1,
        .wake_cb = on_factory_reset_button,
#endif
    };
    pm_init(&pm_cfg);
    
    /* Initialize NVS (required for BLE and state) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS init failed (%s), erasing", esp_err_to_name(ret));
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

    /* Initialize relay */
    relay_t *relay = relay_init();
    if (!relay) {
        ESP_LOGE(TAG, "Failed to initialize relay");
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

        comms_report_t report = {
            .temperature_c = temp,
            .humidity_pct = hum,
            .relay_state = relay_get_state(relay)
        };
        comms_send_report_for(&report, REPORT_DURATION_MS);

        /* Scan for commands from hub */
        comms_message_t msgs[4];
        int count = comms_scan_for(COMMAND_SCAN_DURATION_MS, msgs, 4);
        for (int i = 0; i < count; i++) {
            if (msgs[i].has_relay_cmd) {
                comms_relay_cmd_t *cmd = &msgs[i].relay_cmd;
                /* Check if command is for this device */
                if (strcmp(cmd->device_id, device_name) == 0) {
                    ESP_LOGI(TAG, "Relay command received: %s", cmd->state ? "ON" : "OFF");
                    relay_set(relay, cmd->state);
                }
            }
        }

        comms_close();

        status_set_busy(false);

        /* Sleep until next interval (accounts for time spent working) */
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(MAIN_LOOP_INTERVAL_MS));
    }
}
