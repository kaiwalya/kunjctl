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

To regenerate proto files after editing `.proto` schemas:

```bash
cd node/src/proto
nanopb_generator messages.proto
```

## Project Structure

```
home_automation/
├── node/                       # Node firmware (sensors, BLE advertising)
│   ├── src/
│   │   ├── main.c              # Application entry point
│   │   ├── power_management.c  # PM init and stats logging
│   │   ├── device_name.c       # Deterministic device name generator
│   │   ├── comms/              # BLE communications
│   │   │   └── comms.c/.h      # open/close cycle, send_hello
│   │   ├── inputs/             # Input devices
│   │   │   └── sensors.c/.h    # DHT11 temperature/humidity
│   │   ├── outputs/            # Output devices
│   │   │   └── status.c/.h     # Status LED (WS2812)
│   │   ├── proto/              # Protocol buffers
│   │   │   ├── messages.proto
│   │   │   └── messages.pb.c/.h
│   │   └── state/              # Persistent state (NVS)
│   │       └── state.c/.h      # Pairing state
│   ├── Kconfig.projbuild       # Inputs/Outputs menuconfig
│   └── CMakeLists.txt
├── sdkconfig.defaults          # ESP-IDF Kconfig defaults
├── CMakeLists.txt
└── xmake.lua
```

## Build Commands

### Initial Setup (run once)

```bash
# Configure all targets
xmake set-target

# Or configure specific target
xmake set-target -s node -c esp32h2
```

### Building

```bash
# Build specific target (recommended)
xmake build node-esp32h2
xmake build node-esp32s3

# Clean
xmake clean node-esp32h2
```

> **Warning**: Do NOT build multiple targets in parallel (`xmake` without a specific target). This causes race conditions and corrupted build artifacts.

### Flashing & Monitoring

```bash
# Flash firmware
xmake flash -s node -c esp32h2

# Serial monitor
xmake monitor -s node -c esp32h2

# Flash and monitor
xmake fm -s node -c esp32h2

# Specify port
xmake flash -s node -c esp32h2 -p /dev/tty.usbserial-0001
```

### Configuration

```bash
# Open menuconfig
xmake menuconfig -s node -c esp32h2

# Show firmware size
xmake size -s node -c esp32h2
```

## Adding Subprojects

1. Add the subproject name to the `subprojects` list in `xmake.lua`
2. Run `xmake set-target -s <newproject>` to configure

## Supported Targets

| Chip | Description |
|------|-------------|
| esp32h2 | ESP32-H2 (RISC-V, BLE 5, IEEE 802.15.4) |
| esp32s3 | ESP32-S3 (Xtensa dual-core, Wi-Fi, BLE 5) |

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
