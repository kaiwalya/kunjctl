# Home Automation - ESP32-H2 Project

## Build System

This project uses **xmake as a task runner** for ESP-IDF. It supports multiple targets (esp32h2, esp32s3) and setups.

### Build Commands

```bash
# Build (select specific setup)
# Format: xmake build <subproject>-<chip>-<name>
xmake build thread-end-device-esp32h2-devkitm

# Flash to device (auto-detect port)
xmake flash -s thread-end-device -c esp32h2

# Flash to specific port
xmake flash -s thread-end-device -c esp32h2 -p /dev/tty.usbserial-0001
 
 # Set target context (if needed)
xmake set-target -s thread-end-device -c esp32h2
```

**Important**: Do NOT build multiple targets in parallel - this causes race conditions and corrupted build artifacts.

## Project Structure

```
home-automation/
├── components/               # Shared components
│   ├── thread_comms/         # Thread communications
│   │   ├── thread_comms.c/.h
│   │   └── proto/            # Protocol buffers
│   │       ├── messages.proto
│   │       ├── messages.options
│   │       └── messages.pb.c/.h  (generated)
│   ├── device_name/          # Deterministic name from MAC
│   └── power_management/     # PM init, stats, sleep
├── thread-end-device/src/    # Thread End Device firmware
│   ├── main.c
│   ├── inputs/               # Sensors (DHT, etc.)
│   └── outputs/              # Status LED, etc.
├── thread-router/src/        # Thread Router/Border Router firmware
│   └── main.c
├── thread-rcp/src/           # Thread Radio Co-Processor firmware
│   └── main.c
├── setups/                   # Build configurations
└── sdkconfig.defaults
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
    s->sources.dht = GPIO_DISABLED;
#if CONFIG_DHT_ENABLED
    s->sources.dht = CONFIG_DHT_GPIO;
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

### Comms Pattern (Thread/Openthread)

Thread communications (`thread_comms`) handle the lifecycle of the OpenThread stack.

```c
thread_comms_init();       // Initialize OpenThread stack
thread_comms_start();      // Start Thread network
```

### Sensor Sources Pattern

Support multiple optional sensors with runtime dispatch:

```c
typedef struct {
    int dht;             // GPIO or -1 if disabled
    int soil_moisture;   // Future sensors...
} sensor_sources_t;

esp_err_t sensors_read(sensors_t *s) {
    if (s->sources.dht != GPIO_DISABLED) {
        read_dht(s);
    }
    // Add more sensors here
    return ESP_OK;
}
```

## Configuration

### Kconfig (menuconfig)

Inputs and outputs are configured via `thread-end-device/Kconfig.projbuild`:
- All default to disabled (`default n`)
- Use `menuconfig` items for expandable sections
- GPIO pins configurable per-device

### sdkconfig.defaults

Power management and Thread settings:
- FreeRTOS tickless idle enabled
- Light sleep enabled (for end devices)

## Hardware Notes

- **ESP32-H2**: Native IEEE 802.15.4 support. Use USB-UART bridge for light sleep compatibility.
- **ESP32-S3**: Host for Border Router (requires RCP).

## Timing

Use `vTaskDelayUntil()` for consistent loop intervals that account for work time:

```c
TickType_t last_wake = xTaskGetTickCount();
for (;;) {
    // do work...
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(INTERVAL_MS));
}
```

## Protocol Buffers (nanopb)

Messages are defined in `components/thread_comms/proto/messages.proto` and compiled with nanopb.

### Files

- `messages.proto` - Message definitions
- `messages.options` - nanopb options (max string sizes, etc.)
- `messages.pb.c/.h` - Generated files (checked into repo)

### Regenerating after .proto changes

```bash
xmake codegen
```

Or manually:

```bash
cd components/thread_comms/proto
nanopb_generator messages.proto
```

### Options file format

The `.options` file constrains sizes for embedded use:

```
# Field-specific options
Hello.device_id  max_size:32
```

### Usage pattern

nanopb is a private dependency of the thread_comms component. The thread_comms module exposes mirror types to avoid leaking nanopb into public headers:

```c
// thread_comms.h - public types (no nanopb dependency)
typedef enum { COMMS_SOURCE_NODE, COMMS_SOURCE_HUB } comms_source_t;
typedef struct {
    comms_source_t source_type;
    char device_id[32];
} comms_hello_t;

// thread_comms.c - converts between nanopb Hello and comms_hello_t internally
```
