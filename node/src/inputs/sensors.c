#include "sensors.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "dht.h"
#include <stdlib.h>
#include <stdbool.h>

static const char *TAG = "sensors";

#define GPIO_DISABLED -1

/* Sensor sources - GPIO pins for each sensor type */
typedef struct {
    int dht11;
} sensor_sources_t;

struct sensors_t {
    sensor_sources_t sources;

    /* Sensor values */
    float temperature;
    float humidity;

    /* Availability flags */
    bool has_temperature;
    bool has_humidity;
};

/*── Internal readers ──*/

static void read_dht11(sensors_t *s) {
    float t, h;
    if (dht_read_float_data(DHT_TYPE_DHT11, s->sources.dht11, &h, &t) == ESP_OK) {
        s->temperature = t;
        s->humidity = h;
        s->has_temperature = true;
        s->has_humidity = true;
    } else {
        ESP_LOGE(TAG, "DHT11 read failed");
    }
}

/*── Public API ──*/

sensors_t *sensors_init(void) {
    sensors_t *s = calloc(1, sizeof(sensors_t));
    if (!s) return NULL;

    /* Initialize all sources as disabled */
    s->sources.dht11 = GPIO_DISABLED;

    /* Configure sources based on Kconfig (only #ifdefs in this file) */
#if CONFIG_DHT11_ENABLED
    s->sources.dht11 = CONFIG_DHT11_GPIO;
    ESP_LOGI(TAG, "DHT11 on GPIO %d", s->sources.dht11);
#endif

    return s;
}

void sensors_deinit(sensors_t *sensors) {
    free(sensors);
}

esp_err_t sensors_read(sensors_t *sensors) {
    if (sensors->sources.dht11 != GPIO_DISABLED) {
        read_dht11(sensors);
    }
    return ESP_OK;
}

const float *sensors_get_temperature(sensors_t *sensors) {
    return sensors->has_temperature ? &sensors->temperature : NULL;
}

const float *sensors_get_humidity(sensors_t *sensors) {
    return sensors->has_humidity ? &sensors->humidity : NULL;
}
