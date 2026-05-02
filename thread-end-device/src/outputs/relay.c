#include "relay.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>

static const char *TAG = "relay";

#define GPIO_DISABLED -1

struct relay_t {
    int gpio;        /* Level-driven: signal pin. Latching: SET coil. */
    int reset_gpio;  /* Latching only: RESET coil. GPIO_DISABLED otherwise. */
    bool state;
    bool has_state;
};

#if CONFIG_RELAY_ENABLED && CONFIG_RELAY_TYPE_LATCHING
/*
 * Pulse exactly one coil. The two coils must never be energized at the same
 * time, so we drive the target high, delay, then drive it low again before
 * returning. The other coil is left untouched (already at 0 from init).
 */
static void pulse_coil(int coil_gpio) {
    gpio_set_level(coil_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_RELAY_PULSE_MS));
    gpio_set_level(coil_gpio, 0);
}
#endif

relay_t *relay_init(bool initial_state, bool force_resync) {
    relay_t *r = calloc(1, sizeof(relay_t));
    if (!r) return NULL;

    r->gpio = GPIO_DISABLED;
    r->reset_gpio = GPIO_DISABLED;

#if CONFIG_RELAY_ENABLED
#if CONFIG_RELAY_TYPE_LEVEL
    r->gpio = CONFIG_RELAY_GPIO;
    gpio_hold_dis(r->gpio);
    gpio_set_direction(r->gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(r->gpio, initial_state ? 1 : 0);
    gpio_hold_en(r->gpio);
    ESP_LOGI(TAG, "Level relay on GPIO %d (initial: %s)",
             r->gpio, initial_state ? "ON" : "OFF");
#elif CONFIG_RELAY_TYPE_LATCHING
    r->gpio = CONFIG_RELAY_SET_GPIO;
    r->reset_gpio = CONFIG_RELAY_RESET_GPIO;

    /* Both coils start de-energized. Order matters: configure as outputs at
     * level 0 before doing anything else, so we never glitch high. */
    gpio_set_direction(r->gpio, GPIO_MODE_OUTPUT);
    gpio_set_direction(r->reset_gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(r->gpio, 0);
    gpio_set_level(r->reset_gpio, 0);

    /* On cold boot the relay's mechanical state is unknown (it survived the
     * power loss in whatever position it was in), so we must pulse to force
     * agreement with `initial_state`. On deep-sleep wake the relay is still
     * in the position we left it, so skipping the pulse saves coil wear. */
    if (force_resync) {
        pulse_coil(initial_state ? r->gpio : r->reset_gpio);
        ESP_LOGI(TAG, "Latching relay on SET=%d RESET=%d (resynced to %s)",
                 r->gpio, r->reset_gpio, initial_state ? "ON" : "OFF");
    } else {
        ESP_LOGI(TAG, "Latching relay on SET=%d RESET=%d (trusting %s)",
                 r->gpio, r->reset_gpio, initial_state ? "ON" : "OFF");
    }
#endif

    r->state = initial_state;
    r->has_state = true;
#endif

    return r;
}

void relay_deinit(relay_t *relay) {
    free(relay);
}

void relay_set(relay_t *relay, bool on) {
    if (relay->gpio == GPIO_DISABLED) return;

#if CONFIG_RELAY_ENABLED && CONFIG_RELAY_TYPE_LATCHING
    /* Skip the pulse if we're already in the requested state — saves coil
     * wear on a relay rated for a finite number of mechanical operations. */
    if (relay->has_state && relay->state == on) {
        return;
    }
    pulse_coil(on ? relay->gpio : relay->reset_gpio);
#else
    gpio_hold_dis(relay->gpio);
    gpio_set_level(relay->gpio, on ? 1 : 0);
    gpio_hold_en(relay->gpio);
#endif

    relay->state = on;
    ESP_LOGI(TAG, "Relay set to %s", on ? "ON" : "OFF");
}

const bool *relay_get_state(relay_t *relay) {
    return relay->has_state ? &relay->state : NULL;
}
