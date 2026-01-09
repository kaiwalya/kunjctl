#include "relay.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include <stdlib.h>

static const char *TAG = "relay";

#define GPIO_DISABLED -1

struct relay_t {
    int gpio;
    bool state;
    bool has_state;
};

relay_t *relay_init(bool initial_state) {
    relay_t *r = calloc(1, sizeof(relay_t));
    if (!r) return NULL;

    r->gpio = GPIO_DISABLED;

#if CONFIG_RELAY_ENABLED
    r->gpio = CONFIG_RELAY_GPIO;
    /* Configure GPIO and set initial state */
    gpio_hold_dis(r->gpio);
    gpio_set_direction(r->gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(r->gpio, initial_state ? 1 : 0);
    gpio_hold_en(r->gpio);

    r->state = initial_state;
    r->has_state = true;
    ESP_LOGI(TAG, "Relay on GPIO %d (initial state: %s)", r->gpio, initial_state ? "ON" : "OFF");
#endif

    return r;
}

void relay_deinit(relay_t *relay) {
    free(relay);
}

void relay_set(relay_t *relay, bool on) {
    if (relay->gpio == GPIO_DISABLED) return;
    relay->state = on;
    gpio_hold_dis(relay->gpio);
    gpio_set_level(relay->gpio, on ? 1 : 0);
    gpio_hold_en(relay->gpio);
    ESP_LOGI(TAG, "Relay set to %s", on ? "ON" : "OFF");
}

const bool *relay_get_state(relay_t *relay) {
    return relay->has_state ? &relay->state : NULL;
}
