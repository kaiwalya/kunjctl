#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "dht.h"
#include "power_management.h"

static const char *TAG = "main";

#define DHT_GPIO 12

static void pm_stats_task(void *arg) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        pm_log_stats();
    }
}

static void dht_task(void *arg) {
    float t, h;
    for (;;) {
        if (dht_read_float_data(DHT_TYPE_DHT11, DHT_GPIO, &h, &t) == ESP_OK) {
            ESP_LOGI(TAG, "Temperature: %.1f C / %.1f F, Humidity: %.1f %%", t, t * 9.0f / 5.0f + 32.0f, h);
        } else {
            ESP_LOGE(TAG, "Failed to read from DHT sensor");
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void app_main(void)
{
    pm_init();
    xTaskCreate(pm_stats_task, "pm_stats", 4096, NULL, 5, NULL);
    xTaskCreate(dht_task, "dht", 4096, NULL, 5, NULL);
}
