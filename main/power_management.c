#include "power_management.h"
#include "esp_log.h"
#include "esp_pm.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "pm";

void pm_init(void) {
    esp_pm_config_t config = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = CONFIG_XTAL_FREQ,
        .light_sleep_enable = true
    };
    esp_err_t err = esp_pm_configure(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure PM: %s", esp_err_to_name(err));
    }
}

void pm_log_stats(void) {
    char buf[1024];
    FILE *f = fmemopen(buf, sizeof(buf), "w");
    if (!f) return;
    esp_pm_dump_locks(f);
    fclose(f);

    ESP_LOGI(TAG, "========== Power Stats ==========");
    char *line = buf;
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
