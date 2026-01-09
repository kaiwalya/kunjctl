#include "status.h"
#include "sdkconfig.h"

#if CONFIG_STATUS_LED_ENABLED
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "led_strip.h"

static led_strip_handle_t led = NULL;

void status_init(void) {
    led_strip_config_t cfg = {
        .strip_gpio_num = CONFIG_STATUS_LED_GPIO,
        .max_leds = 1,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&cfg, &rmt_cfg, &led));
    led_strip_clear(led);
}

void status_set_busy(bool busy) {
    if (busy) {
        led_strip_set_pixel(led, 0, 16, 0, 0);  /* Red */
    } else {
        led_strip_clear(led);
    }
    led_strip_refresh(led);
}

void status_it_worked(void) {
    for (int i = 0; i < 3; i++) {
        led_strip_set_pixel(led, 0, 0, 16, 0);  /* Green */
        led_strip_refresh(led);
        vTaskDelay(pdMS_TO_TICKS(150));
        led_strip_clear(led);
        led_strip_refresh(led);
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

#else
/* No-op implementations when LED is disabled */
void status_init(void) {}
void status_set_busy(bool busy) { (void)busy; }
void status_it_worked(void) {}
#endif
