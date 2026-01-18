#include "bridge_state.hpp"

#include "esp_log.h"
#include "esp_timer.h"

#include <esp_matter_cluster.h>
#include <esp_matter_attribute.h>
#include <esp_matter_endpoint.h>

#include <platform/CHIPDeviceLayer.h>

static const char *TAG = "tr-bridge";

using namespace esp_matter;
using namespace esp_matter::cluster;

// Determine device type based on capabilities
// Returns a real Matter device type ID for primary function
static uint32_t get_device_type_for_report(const thread_comms_report_t *report)
{
    // Prioritize relay (plug) if present, otherwise use temperature sensor
    if (report->has_relay_state) {
        return ESP_MATTER_ON_OFF_PLUG_IN_UNIT_DEVICE_TYPE_ID;
    } else {
        return ESP_MATTER_TEMPERATURE_SENSOR_DEVICE_TYPE_ID;
    }
}

// Initialize cluster callbacks for a dynamically created endpoint
// This is needed because provider::Startup() only runs once at Matter init,
// so bridged endpoints created later need their cluster callbacks manually invoked
// Must be called with CHIP stack lock held
static void init_endpoint_cluster_callbacks(endpoint_t *ep)
{
    uint16_t endpoint_id = endpoint::get_id(ep);

    // Acquire CHIP stack lock - cluster init callbacks may trigger reporting
    chip::DeviceLayer::PlatformMgr().LockChipStack();

    cluster_t *cluster = cluster::get_first(ep);

    while (cluster) {
        uint8_t flags = cluster::get_flags(cluster);

        // Call the init callback (e.g., ESPMatterDescriptorClusterServerInitCallback)
        cluster::initialization_callback_t init_callback = cluster::get_init_callback(cluster);
        if (init_callback) {
            ESP_LOGD(TAG, "Calling init callback for cluster on endpoint %u", endpoint_id);
            init_callback(endpoint_id);
        }

        // Call the init function if CLUSTER_FLAG_INIT_FUNCTION is set
        if ((flags & CLUSTER_FLAG_SERVER) && (flags & CLUSTER_FLAG_INIT_FUNCTION)) {
            cluster::function_cluster_init_t init_function =
                (cluster::function_cluster_init_t)cluster::get_function(cluster, CLUSTER_FLAG_INIT_FUNCTION);
            if (init_function) {
                init_function(endpoint_id);
            }
        }

        cluster = cluster::get_next(cluster);
    }

    chip::DeviceLayer::PlatformMgr().UnlockChipStack();
}

esp_err_t BridgeState::device_type_callback(endpoint_t *ep,
                                            uint32_t device_type_id,
                                            void *priv_data)
{
    esp_err_t err = ESP_OK;
    ESP_LOGI(TAG, "Device type callback: device_type=0x%lx", (unsigned long)device_type_id);

    // Get the BridgeDevice from priv_data
    BridgeDevice *dev = static_cast<BridgeDevice *>(priv_data);

    // Add clusters based on the Matter device type
    // The device_type_id is now a real Matter device type
    switch (device_type_id) {
        case ESP_MATTER_TEMPERATURE_SENSOR_DEVICE_TYPE_ID: {
            // Add temperature sensor clusters
            endpoint::temperature_sensor::config_t temp_config;
            err = endpoint::temperature_sensor::add(ep, &temp_config);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to add temperature_sensor: %s", esp_err_to_name(err));
            }

            // Also add humidity sensor for combo devices
            endpoint::humidity_sensor::config_t humidity_config;
            err = endpoint::humidity_sensor::add(ep, &humidity_config);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to add humidity_sensor: %s", esp_err_to_name(err));
            }
            break;
        }

        case ESP_MATTER_ON_OFF_PLUG_IN_UNIT_DEVICE_TYPE_ID: {
            // Add on/off plug clusters
            endpoint::on_off_plug_in_unit::config_t plug_config;
            err = endpoint::on_off_plug_in_unit::add(ep, &plug_config);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to add on_off_plug_in_unit: %s", esp_err_to_name(err));
            }

            // Also add sensors if this is a combo device (sensor + relay)
            if (dev && (dev->persisted.temperature.has_value() || dev->persisted.humidity.has_value())) {
                endpoint::temperature_sensor::config_t temp_config;
                err = endpoint::temperature_sensor::add(ep, &temp_config);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to add temperature_sensor to plug: %s", esp_err_to_name(err));
                }

                endpoint::humidity_sensor::config_t humidity_config;
                err = endpoint::humidity_sensor::add(ep, &humidity_config);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to add humidity_sensor to plug: %s", esp_err_to_name(err));
                }
            }
            break;
        }

        default:
            ESP_LOGW(TAG, "Unknown device type: 0x%lx", (unsigned long)device_type_id);
            break;
    }

    // Set the node label from device_id
    if (dev && !dev->persisted.device_id.empty()) {
        cluster_t *bdbi_cluster = cluster::get(ep, chip::app::Clusters::BridgedDeviceBasicInformation::Id);
        if (bdbi_cluster) {
            char label[33];
            strncpy(label, dev->persisted.device_id.c_str(), sizeof(label) - 1);
            label[sizeof(label) - 1] = '\0';
            bridged_device_basic_information::attribute::create_node_label(bdbi_cluster, label, strlen(label));
        }
    }

    return ESP_OK;
}

esp_err_t BridgeState::init(node_t *node, uint16_t aggregator_endpoint_id)
{
    node_ = node;
    aggregator_endpoint_id_ = aggregator_endpoint_id;

    // Initialize esp_matter_bridge with our device type callback
    esp_err_t err = esp_matter_bridge::initialize(node, device_type_callback);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize esp_matter_bridge: %s", esp_err_to_name(err));
        return err;
    }

    // Load all devices from our NVS
    auto persisted = bridge_nvs_load_all_devices();
    ESP_LOGI(TAG, "Resuming %zu devices from NVS", persisted.size());

    for (auto &p : persisted) {
        BridgeDevice dev;
        dev.persisted = std::move(p);
        dev.matter_device = nullptr;
        dev.last_seen_ms = 0;
        dev.cmd_pending = false;
        dev.cmd_relay_state = false;

        // Resume Matter endpoint using stored endpoint_id
        resume_matter_endpoint(dev);

        devices_.push_back(std::move(dev));
    }

    return ESP_OK;
}

void BridgeState::resume_matter_endpoint(BridgeDevice &dev)
{
    ESP_LOGI(TAG, "Resuming device '%s' at endpoint %u",
             dev.persisted.device_id.c_str(), dev.persisted.endpoint_id);

    dev.matter_device = esp_matter_bridge::resume_device(
        node_,
        dev.persisted.endpoint_id,
        &dev  // priv_data for callback
    );

    if (!dev.matter_device) {
        ESP_LOGE(TAG, "Failed to resume endpoint %u for device '%s'",
                 dev.persisted.endpoint_id, dev.persisted.device_id.c_str());
    } else {
        // Enable the endpoint so it's visible to controllers
        endpoint::enable(dev.matter_device->endpoint);

        // Initialize cluster callbacks (descriptor, etc.) for the dynamic endpoint
        init_endpoint_cluster_callbacks(dev.matter_device->endpoint);
    }
}

void BridgeState::create_matter_endpoint(BridgeDevice &dev, uint32_t device_type_id)
{
    ESP_LOGI(TAG, "Creating endpoint for device '%s' (type=0x%lx)",
             dev.persisted.device_id.c_str(), (unsigned long)device_type_id);

    dev.matter_device = esp_matter_bridge::create_device(
        node_,
        aggregator_endpoint_id_,
        device_type_id,
        &dev  // priv_data for callback
    );

    if (!dev.matter_device) {
        ESP_LOGE(TAG, "Failed to create endpoint for device '%s'",
                 dev.persisted.device_id.c_str());
        return;
    }

    // Get the assigned endpoint ID from the bridge device
    dev.persisted.endpoint_id = dev.matter_device->persistent_info.device_endpoint_id;

    // Enable the endpoint so it's visible to controllers
    endpoint::enable(dev.matter_device->endpoint);

    // Initialize cluster callbacks (descriptor, etc.) for the dynamic endpoint
    init_endpoint_cluster_callbacks(dev.matter_device->endpoint);

    ESP_LOGI(TAG, "Created endpoint %u for device '%s'",
             dev.persisted.endpoint_id, dev.persisted.device_id.c_str());
}

void BridgeState::on_report(const thread_comms_report_t *report)
{
    BridgeDevice *dev = find_by_device_id(report->device_id);

    if (!dev) {
        // New device - create Matter endpoint
        BridgeDevice new_dev;
        new_dev.persisted.device_id = report->device_id;
        new_dev.persisted.endpoint_id = 0;  // Will be assigned by create_device
        new_dev.matter_device = nullptr;
        new_dev.last_seen_ms = 0;
        new_dev.cmd_pending = false;
        new_dev.cmd_relay_state = false;

        // Populate sensor values BEFORE creating endpoint so callback can see them
        if (report->has_temperature) {
            new_dev.persisted.temperature = report->temperature;
        }
        if (report->has_humidity) {
            new_dev.persisted.humidity = report->humidity;
        }
        if (report->has_relay_state) {
            new_dev.persisted.relay_state = report->relay_state;
        }

        // Determine device type from capabilities in report
        uint32_t device_type = get_device_type_for_report(report);
        create_matter_endpoint(new_dev, device_type);

        if (!new_dev.matter_device) {
            ESP_LOGE(TAG, "Failed to create Matter endpoint for '%s'", report->device_id);
            return;
        }

        devices_.push_back(std::move(new_dev));
        dev = &devices_.back();
    } else if (!dev->matter_device) {
        // Device exists in NVS but Matter endpoint failed to resume - recreate it
        ESP_LOGI(TAG, "Recreating Matter endpoint for '%s'", report->device_id);

        uint32_t device_type = get_device_type_for_report(report);
        create_matter_endpoint(*dev, device_type);

        if (!dev->matter_device) {
            ESP_LOGE(TAG, "Failed to recreate Matter endpoint for '%s'", report->device_id);
            return;
        }
    }

    // Update sensor state from report
    if (report->has_temperature) {
        dev->persisted.temperature = report->temperature;
    }
    if (report->has_humidity) {
        dev->persisted.humidity = report->humidity;
    }
    if (report->has_relay_state) {
        dev->persisted.relay_state = report->relay_state;
    }

    dev->last_seen_ms = esp_timer_get_time() / 1000;

    // Persist to NVS
    esp_err_t err = bridge_nvs_save_device(dev->persisted);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save device '%s' to NVS: %s",
                 dev->persisted.device_id.c_str(), esp_err_to_name(err));
    }

    // Update Matter attributes (set flag to prevent callback from queueing commands)
    updating_from_thread = true;
    update_matter_attributes(*dev);
    updating_from_thread = false;

    // Send any pending command
    if (dev->cmd_pending) {
        send_pending_command(*dev);
        dev->cmd_pending = false;
    }
}

void BridgeState::update_matter_attributes(BridgeDevice &dev)
{
    if (!dev.matter_device || !dev.matter_device->endpoint) {
        ESP_LOGW(TAG, "Cannot update attributes - endpoint not ready");
        return;
    }

    endpoint_t *ep = dev.matter_device->endpoint;
    uint16_t endpoint_id = endpoint::get_id(ep);

    ESP_LOGI(TAG, "Updating attributes for endpoint %u", endpoint_id);

    // Update temperature if available
    if (dev.persisted.temperature.has_value()) {
        cluster_t *temp_cluster = cluster::get(ep, chip::app::Clusters::TemperatureMeasurement::Id);
        if (temp_cluster) {
            // Matter temperature is in centidegrees (0.01C units)
            int16_t temp_val = static_cast<int16_t>(dev.persisted.temperature.value() * 100);
            esp_matter_attr_val_t val = esp_matter_nullable_int16(temp_val);
            attribute::update(endpoint_id, chip::app::Clusters::TemperatureMeasurement::Id,
                            chip::app::Clusters::TemperatureMeasurement::Attributes::MeasuredValue::Id, &val);
            ESP_LOGI(TAG, "Updated temperature: %.1fC", dev.persisted.temperature.value());
        } else {
            ESP_LOGW(TAG, "Temperature cluster not found on endpoint %u", endpoint_id);
        }
    }

    // Update humidity if available
    if (dev.persisted.humidity.has_value()) {
        cluster_t *humidity_cluster = cluster::get(ep, chip::app::Clusters::RelativeHumidityMeasurement::Id);
        if (humidity_cluster) {
            // Matter humidity is in 0.01% units
            uint16_t humidity_val = static_cast<uint16_t>(dev.persisted.humidity.value() * 100);
            esp_matter_attr_val_t val = esp_matter_nullable_uint16(humidity_val);
            attribute::update(endpoint_id, chip::app::Clusters::RelativeHumidityMeasurement::Id,
                            chip::app::Clusters::RelativeHumidityMeasurement::Attributes::MeasuredValue::Id, &val);
            ESP_LOGI(TAG, "Updated humidity: %.1f%%", dev.persisted.humidity.value());
        } else {
            ESP_LOGW(TAG, "Humidity cluster not found on endpoint %u", endpoint_id);
        }
    }

    // Update relay state if available
    if (dev.persisted.relay_state.has_value()) {
        cluster_t *on_off_cluster = cluster::get(ep, chip::app::Clusters::OnOff::Id);
        if (on_off_cluster) {
            esp_matter_attr_val_t val = esp_matter_bool(dev.persisted.relay_state.value());
            attribute::update(endpoint_id, chip::app::Clusters::OnOff::Id,
                            chip::app::Clusters::OnOff::Attributes::OnOff::Id, &val);
            ESP_LOGI(TAG, "Updated relay: %s", dev.persisted.relay_state.value() ? "ON" : "OFF");
        } else {
            ESP_LOGW(TAG, "OnOff cluster not found on endpoint %u", endpoint_id);
        }
    }
}

void BridgeState::queue_cmd(uint16_t endpoint_id, bool relay_state)
{
    BridgeDevice *dev = find_by_endpoint_id(endpoint_id);
    if (!dev) {
        ESP_LOGW(TAG, "queue_cmd: endpoint %u not found", endpoint_id);
        return;
    }

    dev->cmd_pending = true;
    dev->cmd_relay_state = relay_state;

    ESP_LOGI(TAG, "Queued command for '%s': relay=%s",
             dev->persisted.device_id.c_str(), relay_state ? "ON" : "OFF");
}

void BridgeState::send_pending_command(BridgeDevice &dev)
{
    ESP_LOGI(TAG, "Sending command to '%s': relay=%s",
             dev.persisted.device_id.c_str(), dev.cmd_relay_state ? "ON" : "OFF");

    // Send command via Thread
    thread_comms_relay_cmd_t cmd = {};
    strncpy(cmd.device_id, dev.persisted.device_id.c_str(), sizeof(cmd.device_id) - 1);
    cmd.relay_state = dev.cmd_relay_state;

    esp_err_t err = thread_comms_send_relay_cmd(&cmd);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send command to '%s': %s",
                 dev.persisted.device_id.c_str(), esp_err_to_name(err));
    }
}

BridgeDevice *BridgeState::find_by_device_id(const char *device_id)
{
    if (!device_id) return nullptr;

    for (auto &dev : devices_) {
        if (dev.persisted.device_id == device_id) {
            return &dev;
        }
    }
    return nullptr;
}

BridgeDevice *BridgeState::find_by_endpoint_id(uint16_t endpoint_id)
{
    for (auto &dev : devices_) {
        if (dev.persisted.endpoint_id == endpoint_id) {
            return &dev;
        }
    }
    return nullptr;
}
