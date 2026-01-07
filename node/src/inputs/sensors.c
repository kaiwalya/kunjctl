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
    int dht;
    dht_sensor_type_t dht_type;
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

static void read_dht(sensors_t *s) {
    float t, h;
    if (dht_read_float_data(s->sources.dht_type, s->sources.dht, &h, &t) == ESP_OK) {
        s->temperature = t;
        s->humidity = h;
        s->has_temperature = true;
        s->has_humidity = true;
    } else {
        ESP_LOGE(TAG, "DHT read failed");
    }
}

/*── Public API ──*/

sensors_t *sensors_init(void) {
    sensors_t *s = calloc(1, sizeof(sensors_t));
    if (!s) return NULL;

    /* Initialize all sources as disabled */
    s->sources.dht = GPIO_DISABLED;

    /* Configure sources based on Kconfig (only #ifdefs in this file) */
#if CONFIG_DHT_ENABLED
    s->sources.dht = CONFIG_DHT_GPIO;
#if CONFIG_DHT_TYPE_DHT22
    s->sources.dht_type = DHT_TYPE_AM2301;
    ESP_LOGI(TAG, "DHT22 on GPIO %d", s->sources.dht);
#else
    s->sources.dht_type = DHT_TYPE_DHT11;
    ESP_LOGI(TAG, "DHT11 on GPIO %d", s->sources.dht);
#endif
#endif

    return s;
}

void sensors_deinit(sensors_t *sensors) {
    free(sensors);
}

esp_err_t sensors_read(sensors_t *sensors) {
    if (sensors->sources.dht != GPIO_DISABLED) {
        read_dht(sensors);
    }
    return ESP_OK;
}

const float *sensors_get_temperature(sensors_t *sensors) {
    return sensors->has_temperature ? &sensors->temperature : NULL;
}

const float *sensors_get_humidity(sensors_t *sensors) {
    return sensors->has_humidity ? &sensors->humidity : NULL;
}
