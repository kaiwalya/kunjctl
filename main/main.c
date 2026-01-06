#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_random.h"
#include "power_management.h"

static void pm_stats_task(void *arg) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        pm_log_stats();
    }
}

void app_main(void)
{
    pm_init();
    xTaskCreate(pm_stats_task, "pm_stats", 4096, NULL, 5, NULL);

    // Simulate ~50% CPU load
    volatile uint32_t dummy = 0;
    for (;;) {
        // Work for 200-400ms (avg 300ms)
        uint32_t work_ms = 200 + (esp_random() % 200);
        uint32_t iterations = work_ms * 10000;  // More iterations per ms
        for (uint32_t i = 0; i < iterations; i++) {
            dummy += i * i;
        }

        // Sleep for 200-400ms (avg 300ms) - roughly equal to work time
        uint32_t sleep_ms = 200 + (esp_random() % 200);
        vTaskDelay(pdMS_TO_TICKS(sleep_ms));
    }
}
