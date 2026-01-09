#include "power_management.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "pm";
static uint32_t stats_interval_ms = 10000;
static pm_wake_cb_t g_wake_cb = NULL;
static QueueHandle_t g_wake_queue = NULL;

/* Store wake GPIO config for deep sleep */
static pm_wake_gpio_t g_wake_gpios[PM_MAX_WAKE_GPIOS];
static uint8_t g_num_wake_gpios = 0;

static void pm_stats_task(void *arg) {
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(stats_interval_ms));
        pm_log_stats();
    }
}

static void IRAM_ATTR gpio_isr_handler(void *arg) {
    gpio_num_t gpio = (gpio_num_t)(uintptr_t)arg;
    xQueueSendFromISR(g_wake_queue, &gpio, NULL);
}

static void pm_wake_task(void *arg) {
    gpio_num_t gpio;
    for (;;) {
        if (xQueueReceive(g_wake_queue, &gpio, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "Wake GPIO%d triggered", gpio);
            if (g_wake_cb) {
                g_wake_cb(gpio);
            }
        }
    }
}

void pm_init(const pm_config_t *config) {
    /* Set stats interval */
    if (config->stats_interval_ms > 0) {
        stats_interval_ms = config->stats_interval_ms;
    }

    /* Store callback */
    g_wake_cb = config->wake_cb;

    /* Store wake GPIO config for deep sleep */
    g_num_wake_gpios = config->num_wake_gpios;
    for (int i = 0; i < config->num_wake_gpios && i < PM_MAX_WAKE_GPIOS; i++) {
        g_wake_gpios[i] = config->wake_gpios[i];
    }

    /* Check if we woke from deep sleep via GPIO */
    esp_sleep_wakeup_cause_t wakeup_cause = esp_sleep_get_wakeup_cause();
    if ((wakeup_cause == ESP_SLEEP_WAKEUP_EXT1 || wakeup_cause == ESP_SLEEP_WAKEUP_GPIO)
        && g_wake_cb != NULL) {
        /* Find which configured GPIO is currently active */
        for (int i = 0; i < g_num_wake_gpios; i++) {
            gpio_num_t gpio = g_wake_gpios[i].gpio;
            int level = gpio_get_level(gpio);
            bool is_active = g_wake_gpios[i].active_low ? (level == 0) : (level == 1);
            if (is_active) {
                ESP_LOGI(TAG, "Woke from deep sleep via GPIO%d", gpio);
                g_wake_cb(gpio);
                break;
            }
        }
    }

    /* Configure GPIO wake sources */
    if (config->num_wake_gpios > 0) {
        /* Create queue and task for handling wake events */
        g_wake_queue = xQueueCreate(4, sizeof(gpio_num_t));
        xTaskCreate(pm_wake_task, "pm_wake", 2048, NULL, 10, NULL);

        /* Install GPIO ISR service */
        gpio_install_isr_service(0);

        for (int i = 0; i < config->num_wake_gpios; i++) {
            const pm_wake_gpio_t *wake_gpio = &config->wake_gpios[i];
            gpio_num_t gpio = wake_gpio->gpio;
            gpio_int_type_t intr_type = wake_gpio->active_low
                ? GPIO_INTR_LOW_LEVEL
                : GPIO_INTR_HIGH_LEVEL;

            /* Configure GPIO as input with appropriate pull */
            gpio_config_t io_cfg = {
                .pin_bit_mask = (1ULL << gpio),
                .mode = GPIO_MODE_INPUT,
                .pull_up_en = wake_gpio->active_low ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
                .pull_down_en = wake_gpio->active_low ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
                .intr_type = intr_type,
            };
            gpio_config(&io_cfg);

            /* Add ISR handler */
            gpio_isr_handler_add(gpio, gpio_isr_handler, (void *)(uintptr_t)gpio);

            /* Enable as wake source for light sleep */
            gpio_wakeup_enable(gpio, intr_type);
            ESP_LOGI(TAG, "GPIO%d configured as wake source (active_%s)",
                     gpio, wake_gpio->active_low ? "low" : "high");
        }

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
    }

    ESP_LOGI(TAG, "Power management configured (light_sleep=%s)",
             config->light_sleep_enable ? "enabled" : "disabled");

    /* Start stats logging task */
    xTaskCreate(pm_stats_task, "pm_stats", 4096, NULL, 5, NULL);
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

void pm_deep_sleep(void) {
    /* Configure GPIO wake sources for deep sleep using EXT1 */
    if (g_num_wake_gpios > 0) {
        uint64_t gpio_mask = 0;
        esp_sleep_ext1_wakeup_mode_t mode = ESP_EXT1_WAKEUP_ANY_LOW;

        for (int i = 0; i < g_num_wake_gpios; i++) {
            gpio_mask |= (1ULL << g_wake_gpios[i].gpio);
            /* Use first GPIO's active_low setting for mode (ESP-IDF limitation: one mode for all) */
            if (i == 0) {
                mode = g_wake_gpios[i].active_low
                    ? ESP_EXT1_WAKEUP_ANY_LOW
                    : ESP_EXT1_WAKEUP_ANY_HIGH;
            }
        }

        esp_err_t err = esp_sleep_enable_ext1_wakeup(gpio_mask, mode);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to enable EXT1 deep sleep wake: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "EXT1 wake configured for deep sleep (mask=0x%llx, mode=%s)",
                     gpio_mask, mode == ESP_EXT1_WAKEUP_ANY_LOW ? "any_low" : "any_high");
        }
    }

    ESP_LOGI(TAG, "Entering deep sleep");
    esp_deep_sleep_start();
    /* Never reached */
}

void pm_restart(void) {
    ESP_LOGI(TAG, "Restarting...");
    esp_restart();
    /* Never reached */
}
