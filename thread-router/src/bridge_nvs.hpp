#pragma once

#include <optional>
#include <string>
#include <vector>
#include <cstdint>

#include "esp_err.h"

// Device state for bridge registry
struct BridgeDeviceState {
    std::string device_id;          // "vivid-falcon-a3f2"
    uint16_t endpoint_id;

    std::optional<float> temperature;
    std::optional<float> humidity;
    std::optional<bool> relay_state;
};

// Initialize NVS namespace for bridge
esp_err_t bridge_nvs_init();

// Allocate next endpoint ID (increments and persists counter)
uint16_t bridge_nvs_alloc_endpoint_id();

// Get current endpoint ID counter without incrementing
uint16_t bridge_nvs_get_next_endpoint_id();

// Device CRUD operations
esp_err_t bridge_nvs_save_device(const BridgeDeviceState &device);
std::optional<BridgeDeviceState> bridge_nvs_load_device(const char *hex_suffix);
esp_err_t bridge_nvs_delete_device(const char *hex_suffix);

// Load all devices from NVS
std::vector<BridgeDeviceState> bridge_nvs_load_all_devices();

// Erase all bridge data from NVS (keeps Matter pairing intact)
esp_err_t bridge_nvs_erase_all();

// Extract hex suffix from device_id: "vivid-falcon-a3f2" -> "a3f2"
// Returns pointer to suffix within device_id, or nullptr if invalid format
const char *bridge_nvs_get_hex_suffix(const char *device_id);
