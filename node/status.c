#include "status.h"
#include "sdkconfig.h"

#if CONFIG_NODE_LED_ENABLED
#include "esp_check.h"
#include "led_strip.h"

static led_strip_handle_t led = NULL;

void status_init(void) {
    led_strip_config_t cfg = {
        .strip_gpio_num = CONFIG_NODE_LED_GPIO,
        .max_leds = 1,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&cfg, &rmt_cfg, &led));
    led_strip_clear(led);
}

void status_set(uint8_t r, uint8_t g, uint8_t b) {
    led_strip_set_pixel(led, 0, r, g, b);
    led_strip_refresh(led);
}

void status_off(void) {
    led_strip_clear(led);
    led_strip_refresh(led);
}

#else
/* No-op implementations when LED is disabled */
void status_init(void) {}
void status_set(uint8_t r, uint8_t g, uint8_t b) { (void)r; (void)g; (void)b; }
void status_off(void) {}
#endif
