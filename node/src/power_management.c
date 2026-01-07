#include "power_management.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "pm";
static uint32_t stats_interval_ms = 10000;

static void pm_stats_task(void *arg) {
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(stats_interval_ms));
        pm_log_stats();
    }
}

void pm_init(const pm_config_t *config) {
    /* Set stats interval */
    if (config->stats_interval_ms > 0) {
        stats_interval_ms = config->stats_interval_ms;
    }

    /* Configure GPIO wake sources */
    for (int i = 0; i < config->num_wake_gpios; i++) {
        gpio_num_t gpio = config->wake_gpios[i];
        gpio_wakeup_enable(gpio, GPIO_INTR_LOW_LEVEL);
        ESP_LOGI(TAG, "GPIO%d configured as wake source", gpio);
    }

    if (config->num_wake_gpios > 0) {
        esp_sleep_enable_gpio_wakeup();
    }

    /* Configure power management */
    esp_pm_config_t pm_cfg = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = CONFIG_XTAL_FREQ,
        .light_sleep_enable = config->light_sleep_enable
    };
    esp_err_t err = esp_pm_configure(&pm_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure PM: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Power management configured (light_sleep=%d)", config->light_sleep_enable);
    }

    /* Start stats logging task */
    xTaskCreate(pm_stats_task, "power_manager", 4096, NULL, 5, NULL);
}

static void log_multiline(const char *buf) {
    char *line = (char *)buf;
    while (line && *line) {
        char *eol = strchr(line, '\n');
        int len = eol ? (eol - line) : strlen(line);
        if (len > 0) {
            ESP_LOGI(TAG, "%.*s", len, line);
        }
        if (!eol) break;
        line = eol + 1;
    }
}

void pm_log_stats(void) {
    char buf[1024];

    /* Print PM locks */
    FILE *f = fmemopen(buf, sizeof(buf), "w");
    if (f) {
        esp_pm_dump_locks(f);
        fclose(f);
        ESP_LOGI(TAG, "========== Power Stats ==========");
        log_multiline(buf);
    }

    /* Print FreeRTOS task list */
    ESP_LOGI(TAG, "========== Tasks ==========");
    ESP_LOGI(TAG, "Name            State   Prio    Stack   Num");
    vTaskList(buf);
    log_multiline(buf);
}
