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

relay_t *relay_init(void) {
    relay_t *r = calloc(1, sizeof(relay_t));
    if (!r) return NULL;

    r->gpio = GPIO_DISABLED;

#if CONFIG_RELAY_ENABLED
    r->gpio = CONFIG_RELAY_GPIO;
    gpio_reset_pin(r->gpio);
    gpio_set_direction(r->gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(r->gpio, 0);
    r->state = false;
    r->has_state = true;
    ESP_LOGI(TAG, "Relay on GPIO %d", r->gpio);
#endif

    return r;
}

void relay_deinit(relay_t *relay) {
    free(relay);
}

void relay_set(relay_t *relay, bool on) {
    if (relay->gpio == GPIO_DISABLED) return;
    relay->state = on;
    gpio_set_level(relay->gpio, on ? 1 : 0);
}

const bool *relay_get_state(relay_t *relay) {
    return relay->has_state ? &relay->state : NULL;
}
