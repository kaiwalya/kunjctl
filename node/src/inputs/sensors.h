#pragma once

#include "esp_err.h"

typedef struct sensors_t sensors_t;

sensors_t *sensors_init(void);
void sensors_deinit(sensors_t *sensors);

/* Read all configured sensors - always returns ESP_OK, individual failures logged */
esp_err_t sensors_read(sensors_t *sensors);

/* Getters - return NULL if sensor not configured or read failed */
const float *sensors_get_temperature(sensors_t *sensors);
const float *sensors_get_humidity(sensors_t *sensors);
