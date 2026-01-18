#pragma once

#include "bridge_nvs.hpp"

#include <vector>
#include <cstdint>

#include <esp_matter.h>
#include <esp_matter_bridge.h>

extern "C" {
#include "thread_comms.h"
}

// Each Thread device maps to up to 3 Matter endpoints:
// - On/Off Plug-in Unit (for relay control)
// - Temperature Sensor
// - Humidity Sensor

struct BridgeDevice {
    BridgeDeviceState persisted;    // From our NVS

    // Runtime only - Matter device handles for each capability
    esp_matter_bridge::device_t *plug_device = nullptr;
    esp_matter_bridge::device_t *temp_device = nullptr;
    esp_matter_bridge::device_t *humidity_device = nullptr;

    int64_t last_seen_ms = 0;
    bool cmd_pending = false;
    bool cmd_relay_state = false;
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
    BridgeDevice *find_by_plug_endpoint(uint16_t endpoint_id);

    // Device type callback for esp_matter_bridge
    static esp_err_t device_type_callback(esp_matter::endpoint_t *ep,
                                          uint32_t device_type_id,
                                          void *priv_data);

private:
    esp_matter::node_t *node_;
    uint16_t aggregator_endpoint_id_;
    std::vector<BridgeDevice> devices_;

    // Matter endpoint lifecycle - creates/resumes all endpoints for a device
    void create_endpoints_for_device(BridgeDevice &dev, const thread_comms_report_t *report);
    void resume_endpoints_for_device(BridgeDevice &dev);

    // Single endpoint helpers
    esp_matter_bridge::device_t *create_single_endpoint(BridgeDevice &dev, uint32_t device_type_id, const char *label_suffix);
    esp_matter_bridge::device_t *resume_single_endpoint(BridgeDevice &dev, uint16_t endpoint_id, const char *label_suffix);

    // Attribute updates
    void update_matter_attributes(BridgeDevice &dev);

    // Command delivery
    void send_pending_command(BridgeDevice &dev);
};
