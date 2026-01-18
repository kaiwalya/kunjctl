# home_automation

ESP-IDF project targeting ESP32-H2 and ESP32-S3.

## Prerequisites

### ESP-IDF SDK

This project requires [ESP-IDF](https://github.com/espressif/esp-idf) v5.5.2 or later.

The easiest way to install and manage ESP-IDF is using the [ESP-IDF Installation Manager UI](https://docs.espressif.com/projects/idf-im-ui/en/latest/).

After installation, ensure these environment variables are set:
- `IDF_PATH` - Path to ESP-IDF installation
- `IDF_PYTHON_ENV_PATH` - Path to ESP-IDF Python virtual environment

### xmake

Install [xmake](https://xmake.io/) for build orchestration:

```bash
# macOS
brew install xmake

# Linux
curl -fsSL https://xmake.io/shget.text | bash

# Windows
winget install xmake
```

### Protocol Buffers (for message serialization)

Install protobuf compiler and nanopb generator:

```bash
# macOS
brew install protobuf
pip install nanopb

# Linux
sudo apt install protobuf-compiler
pip install nanopb
```

The generated files (`messages.pb.c/.h`) are checked into the repo. Use `xmake codegen` to regenerate after editing `.proto` schemas.

## Project Structure

```
home_automation/
├── components/                   # Shared components
│   ├── thread_comms/             # Thread communications
│   │   ├── thread_comms.c/.h
│   │   └── proto/                # Protocol buffers
│   │       ├── messages.proto
│   │       ├── messages.options
│   │       └── messages.pb.c/.h  (generated)
│   ├── device_name/              # Deterministic device name
│   └── power_management/         # PM init, stats, sleep
├── thread-end-device/            # Thread End Device firmware
│   ├── src/
│   │   ├── main.c
│   │   └── ...
│   └── Kconfig.projbuild
├── thread-router/                # Thread Router/Border Router firmware
│   └── src/main.c
├── thread-rcp/                   # Thread Radio Co-Processor firmware
│   └── src/main.c
├── setups/                       # Build configurations (targets)
├── sdkconfig.defaults
└── xmake.lua
```

## Build Commands

### Initial Setup (run once)

```bash
# Configure all targets
xmake set-target

# Or configure specific target (example)
xmake set-target -s thread-end-device -c esp32h2
```

### Building

```bash
# Build specific setup (recommended)
# Syntax: <subproject>-<chip>-<config_name>

# Thread End Device on ESP32-H2 DevKitM
xmake build thread-end-device-esp32h2-devkitm

# Thread Border Router on ESP32-S3
xmake build thread-router-esp32s3-thread-border-router

# Thread Radio Co-Processor (RCP) on ESP32-H2
xmake build thread-rcp-esp32h2-thread-border-router

# Clean
xmake clean thread-end-device-esp32h2-devkitm
```

> **Warning**: Do NOT build multiple targets in parallel (`xmake` without a specific target). This causes race conditions and corrupted build artifacts.

### Flashing & Monitoring

The `flash`, `monitor`, and `fm` (flash + monitor) commands accept an optional `-p <port>` parameter to specify the serial port. If omitted, `idf.py` will attempt to auto-detect the port.

```bash
# Flash firmware (auto-detect port)
xmake flash -s thread-end-device -c esp32h2

# Serial monitor (auto-detect port)
xmake monitor -s thread-end-device -c esp32h2

# Flash and monitor (auto-detect port)
xmake fm -s thread-end-device -c esp32h2

# Explicitly specify port (recommended if multiple devices connected)
xmake flash -s thread-end-device -c esp32h2 -p /dev/tty.usbserial-0001
xmake monitor -s thread-end-device -c esp32h2 -p /dev/tty.usbserial-0001
xmake fm -s thread-end-device -c esp32h2 -p /dev/tty.usbserial-0001
```

### Configuration

```bash
# Open menuconfig
xmake menuconfig -s thread-end-device -c esp32h2

# Show firmware size
xmake size -s thread-end-device -c esp32h2
```

### Code Generation

```bash
# Regenerate protobuf files after editing .proto schemas
xmake codegen
```

## Adding Subprojects

1. Add the subproject name to the `subprojects` list in `xmake.lua`
2. Run `xmake set-target -s <newproject>` to configure

## Supported Targets

| Chip | Description |
|------|-------------|
| esp32h2 | ESP32-H2 (RISC-V, Thread/Zigbee/BLE 5, IEEE 802.15.4) |
| esp32s3 | ESP32-S3 (Xtensa dual-core, Wi-Fi, BLE 5, Thread Border Router Host) |

## Power Management

Power management is configured via `sdkconfig.defaults`:

- FreeRTOS runtime stats enabled
- Tickless idle enabled
- PM profiling enabled
- Dynamic frequency scaling (max CPU freq down to XTAL freq)
- Light sleep enabled

### Hardware Notes

- **ESP32-H2**: Has both native USB and USB-UART bridge. Use USB-UART for light sleep compatibility.
- **ESP32-S3**: Only has native USB-Serial/JTAG which disconnects during light sleep cycles.
