# Home Automation - ESP32-H2 Project

## Build System

This project uses **xmake as a task runner** for ESP-IDF. It supports multiple targets (esp32h2, esp32s3).

### Build Commands

```bash
# Build for ESP32-H2 (preferred)
xmake build node-esp32h2

# Flash to device
xmake flash -s node -c esp32h2

# Set target context (if needed)
xmake set-target -s node -c esp32h2
```

**Important**: Do NOT build multiple targets in parallel - this causes race conditions and corrupted build artifacts.

## Project Structure

```
node/src/
├── main.c                  # Application entry, main loop
├── power_management.c/.h   # PM init and stats
├── device_name.c/.h        # Deterministic name from MAC
├── comms/                  # BLE communications domain
│   └── comms.c/.h
├── inputs/                 # Input devices domain
│   └── sensors.c/.h
├── outputs/                # Output devices domain
│   └── status.c/.h
├── proto/                  # Protocol buffers
│   └── messages.pb.c/.h
└── state/                  # Persistent state domain
    └── state.c/.h
```

## Architecture Patterns

### Domain-Driven Design (DDD) in C

Each domain module follows a consistent pattern:

**Header (.h) - Public interface only:**
```c
#pragma once
#include "esp_err.h"

typedef struct sensors_t sensors_t;  // Opaque handle

sensors_t *sensors_init(void);
void sensors_deinit(sensors_t *sensors);
esp_err_t sensors_read(sensors_t *sensors);
const float *sensors_get_temperature(sensors_t *sensors);  // NULL if unavailable
```

**Implementation (.c) - Private details:**
```c
struct sensors_t {
    sensor_sources_t sources;  // GPIO config set at init
    float temperature;
    bool has_temperature;
};

// #ifdefs only in init, runtime checks elsewhere
sensors_t *sensors_init(void) {
    sensors_t *s = calloc(1, sizeof(sensors_t));
    s->sources.dht11 = GPIO_DISABLED;
#if CONFIG_DHT11_ENABLED
    s->sources.dht11 = CONFIG_DHT11_GPIO;
#endif
    return s;
}
```

**Key principles:**
- Opaque handles (`typedef struct foo_t foo_t`) hide internals
- `init()` returns handle, `deinit()` frees it
- Getters return `const T*` for optional values (NULL = unavailable)
- `#ifdef` only in init, use runtime checks elsewhere
- Store GPIO/config in struct at init time
- Errors via `esp_err_t` return codes (C has no exceptions)
- `calloc` for zero-init, check for NULL

### Comms Pattern (Radio Lifecycle)

For power-sensitive BLE operations:

```c
comms_init(device_id);     // One-time: encode messages, store config

for (;;) {
    comms_open();          // Start radio
    comms_send_hello();    // Broadcast
    // comms_wait_for_input();
    comms_close();         // Stop radio, save power

    sleep();
}
```

### Sensor Sources Pattern

Support multiple optional sensors with runtime dispatch:

```c
typedef struct {
    int dht11;           // GPIO or -1 if disabled
    int soil_moisture;   // Future sensors...
} sensor_sources_t;

esp_err_t sensors_read(sensors_t *s) {
    if (s->sources.dht11 != GPIO_DISABLED) {
        read_dht11(s);
    }
    // Add more sensors here
    return ESP_OK;
}
```

## Configuration

### Kconfig (menuconfig)

Inputs and outputs are configured via `node/Kconfig.projbuild`:
- All default to disabled (`default n`)
- Use `menuconfig` items for expandable sections
- GPIO pins configurable per-device

### sdkconfig.defaults

Power management and BLE settings:
- FreeRTOS tickless idle enabled
- Light sleep enabled
- NimBLE with extended advertising and 2M PHY
- NimBLE log level set to WARNING

## Hardware Notes

- **ESP32-H2**: Use USB-UART bridge (not native USB) for light sleep compatibility
- **ESP32-S3**: Native USB disconnects during light sleep

## Timing

Use `vTaskDelayUntil()` for consistent loop intervals that account for work time:

```c
TickType_t last_wake = xTaskGetTickCount();
for (;;) {
    // do work...
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(INTERVAL_MS));
}
```
