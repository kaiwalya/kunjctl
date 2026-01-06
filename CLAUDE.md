# Home Automation - ESP32-H2 Project

## Build System

This project uses **xmake as a task runner** for ESP-IDF. It supports multiple targets (esp32h2, esp32s3).

### Build Commands

```bash
# Build for ESP32-H2 (preferred)
xmake build main-esp32h2

# Flash to device
xmake flash -c esp32h2 -s main

# Set target context (if needed)
xmake set-target -c esp32h2
```

**Important**: Do NOT build multiple targets in parallel - this causes race conditions and corrupted build artifacts.

## Project Structure

- `main/main.c` - Application entry point
- `main/power_management.c` - Power management initialization and stats logging
- `main/power_management.h` - PM header
- `sdkconfig.defaults` - ESP-IDF Kconfig defaults

## Configuration

Power management is configured via `sdkconfig.defaults`:
- FreeRTOS runtime stats enabled
- Tickless idle enabled
- PM profiling enabled
- Light sleep enabled

## Hardware Notes

- Target: ESP32-H2
- When light sleep is enabled, use the **USB-UART bridge port** (not native USB-Serial/JTAG) as native USB disconnects during sleep
