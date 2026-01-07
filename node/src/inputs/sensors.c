#include "sensors.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "sensors";

#if CONFIG_DHT11_ENABLED
#include "dht.h"

#define DHT_READ_INTERVAL_MS 10000

static void dht_task(void *arg) {
    float t, h;
    for (;;) {
        if (dht_read_float_data(DHT_TYPE_DHT11, CONFIG_DHT11_GPIO, &h, &t) == ESP_OK) {
            ESP_LOGI(TAG, "DHT11: %.1f C / %.1f F, Humidity: %.1f %%",
                     t, t * 9.0f / 5.0f + 32.0f, h);
        } else {
            ESP_LOGE(TAG, "Failed to read from DHT11 sensor");
        }
        vTaskDelay(pdMS_TO_TICKS(DHT_READ_INTERVAL_MS));
    }
}
#endif

void sensors_init(void) {
#if CONFIG_DHT11_ENABLED
    ESP_LOGI(TAG, "Starting DHT11 on GPIO %d", CONFIG_DHT11_GPIO);
    xTaskCreate(dht_task, "dht", 4096, NULL, 5, NULL);
#endif
}
