#pragma once

#include "driver/gpio.h"
#include <stdint.h>
#include <stdbool.h>

#define PM_MAX_WAKE_GPIOS 4

typedef struct {
    gpio_num_t wake_gpios[PM_MAX_WAKE_GPIOS];
    uint8_t num_wake_gpios;
    bool light_sleep_enable;
    uint32_t stats_interval_ms;
} pm_config_t;

void pm_init(const pm_config_t *config);
void pm_log_stats(void);

/* Enter deep sleep with no wake source (only reset wakes) */
void pm_deep_sleep(void) __attribute__((noreturn));
