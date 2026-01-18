#pragma once

#include "bridge_nvs.hpp"

#include <vector>
#include <cstdint>

#include <esp_matter.h>
#include <esp_matter_bridge.h>

extern "C" {
#include "thread_comms.h"
}

// We use standard Matter device type IDs:
// - ESP_MATTER_TEMPERATURE_SENSOR_DEVICE_TYPE_ID for sensor-only devices
// - ESP_MATTER_ON_OFF_PLUG_IN_UNIT_DEVICE_TYPE_ID for devices with relay

struct BridgeDevice {
    BridgeDeviceState persisted;    // From our NVS

    // Runtime only (not persisted)
    esp_matter_bridge::device_t *matter_device;
    int64_t last_seen_ms;
    bool cmd_pending;
    bool cmd_relay_state;
};

class BridgeState {
public:
    // Initialize bridge state - call after esp_matter::start()
    // aggregator_endpoint_id: the Matter aggregator endpoint ID
    esp_err_t init(esp_matter::node_t *node, uint16_t aggregator_endpoint_id);

    // Called from Thread message callback
    void on_report(const thread_comms_report_t *report);

    // Called from Matter PRE_UPDATE callback for OnOff cluster
    void queue_cmd(uint16_t endpoint_id, bool relay_state);

    // Flag to skip attribute callbacks during our own updates
    bool updating_from_thread = false;

    // Lookup
    BridgeDevice *find_by_device_id(const char *device_id);
    BridgeDevice *find_by_endpoint_id(uint16_t endpoint_id);

    // Device type callback for esp_matter_bridge
    static esp_err_t device_type_callback(esp_matter::endpoint_t *ep,
                                          uint32_t device_type_id,
                                          void *priv_data);

private:
    esp_matter::node_t *node_;
    uint16_t aggregator_endpoint_id_;
    std::vector<BridgeDevice> devices_;

    // Matter endpoint lifecycle
    void create_matter_endpoint(BridgeDevice &dev, uint32_t device_type_id);
    void resume_matter_endpoint(BridgeDevice &dev);

    // Attribute updates
    void update_matter_attributes(BridgeDevice &dev);

    // Command delivery
    void send_pending_command(BridgeDevice &dev);
};
