#pragma once

#include "driver/gpio.h"
#include <stdint.h>
#include <stdbool.h>

#define PM_MAX_WAKE_GPIOS 4

/**
 * Callback invoked when a wake GPIO is triggered.
 * Called from task context (not ISR).
 */
typedef void (*pm_wake_cb_t)(gpio_num_t gpio);

/**
 * Wake GPIO configuration.
 */
typedef struct {
    gpio_num_t gpio;
    bool active_low;      /* true = trigger on low (pull-up), false = trigger on high (pull-down) */
} pm_wake_gpio_t;

typedef struct {
    pm_wake_gpio_t wake_gpios[PM_MAX_WAKE_GPIOS];
    uint8_t num_wake_gpios;
    bool light_sleep_enable;
    uint32_t stats_interval_ms;
    pm_wake_cb_t wake_cb;  /* Optional callback for wake GPIO events */
} pm_config_t;

void pm_init(const pm_config_t *config);
void pm_log_stats(void);

/* Enter deep sleep using configured wake GPIOs */
void pm_deep_sleep(void) __attribute__((noreturn));

/* Restart the device */
void pm_restart(void) __attribute__((noreturn));
