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

// Initialize cluster callbacks for a dynamically created endpoint
// This is needed because provider::Startup() only runs once at Matter init,
// so bridged endpoints created later need their cluster callbacks manually invoked
static void init_endpoint_cluster_callbacks(endpoint_t *ep)
{
    uint16_t endpoint_id = endpoint::get_id(ep);

    chip::DeviceLayer::PlatformMgr().LockChipStack();

    cluster_t *cluster = cluster::get_first(ep);

    while (cluster) {
        uint8_t flags = cluster::get_flags(cluster);

        cluster::initialization_callback_t init_callback = cluster::get_init_callback(cluster);
        if (init_callback) {
            init_callback(endpoint_id);
        }

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

// Set the node label on a bridged endpoint
static void set_endpoint_label(endpoint_t *ep, const char *device_id, const char *suffix = nullptr)
{
    cluster_t *bdbi_cluster = cluster::get(ep, chip::app::Clusters::BridgedDeviceBasicInformation::Id);
    if (bdbi_cluster) {
        char buf[33];
        if (suffix) {
            snprintf(buf, sizeof(buf), "%s %s", device_id, suffix);
        } else {
            strncpy(buf, device_id, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
        }
        bridged_device_basic_information::attribute::create_node_label(bdbi_cluster, buf, strlen(buf));
    }
}

esp_err_t BridgeState::device_type_callback(endpoint_t *ep,
                                            uint32_t device_type_id,
                                            void *priv_data)
{
    esp_err_t err = ESP_OK;

    // Add clusters based on the device type
    switch (device_type_id) {
        case ESP_MATTER_TEMPERATURE_SENSOR_DEVICE_TYPE_ID: {
            endpoint::temperature_sensor::config_t config;
            err = endpoint::temperature_sensor::add(ep, &config);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to add temperature_sensor: %s", esp_err_to_name(err));
            }
            break;
        }

        case ESP_MATTER_HUMIDITY_SENSOR_DEVICE_TYPE_ID: {
            endpoint::humidity_sensor::config_t config;
            err = endpoint::humidity_sensor::add(ep, &config);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to add humidity_sensor: %s", esp_err_to_name(err));
            }
            break;
        }

        case ESP_MATTER_ON_OFF_PLUG_IN_UNIT_DEVICE_TYPE_ID: {
            endpoint::on_off_plug_in_unit::config_t config;
            err = endpoint::on_off_plug_in_unit::add(ep, &config);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to add on_off_plug_in_unit: %s", esp_err_to_name(err));
            }
            break;
        }

        default:
            ESP_LOGW(TAG, "Unknown device type: 0x%lx", (unsigned long)device_type_id);
            break;
    }

    return ESP_OK;
}

esp_err_t BridgeState::init(node_t *node, uint16_t aggregator_endpoint_id)
{
    node_ = node;
    aggregator_endpoint_id_ = aggregator_endpoint_id;

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

        resume_endpoints_for_device(dev);

        devices_.push_back(std::move(dev));
    }

    return ESP_OK;
}

esp_matter_bridge::device_t *BridgeState::create_single_endpoint(BridgeDevice &dev, uint32_t device_type_id, const char *label_suffix)
{
    esp_matter_bridge::device_t *matter_dev = esp_matter_bridge::create_device(
        node_,
        aggregator_endpoint_id_,
        device_type_id,
        nullptr  // priv_data not needed - we set label directly
    );

    if (!matter_dev) {
        ESP_LOGE(TAG, "Failed to create endpoint for '%s' (type=0x%lx)",
                 dev.persisted.device_id.c_str(), (unsigned long)device_type_id);
        return nullptr;
    }

    // Enable and initialize the endpoint
    endpoint::enable(matter_dev->endpoint);
    init_endpoint_cluster_callbacks(matter_dev->endpoint);

    // Set the device label so Google Home shows the Thread device name
    set_endpoint_label(matter_dev->endpoint, dev.persisted.device_id.c_str(), label_suffix);

    uint16_t ep_id = matter_dev->persistent_info.device_endpoint_id;
    ESP_LOGI(TAG, "Created endpoint %u for '%s' (type=0x%lx)",
             ep_id, dev.persisted.device_id.c_str(), (unsigned long)device_type_id);

    return matter_dev;
}

esp_matter_bridge::device_t *BridgeState::resume_single_endpoint(BridgeDevice &dev, uint16_t endpoint_id, const char *label_suffix)
{
    if (endpoint_id == 0) {
        return nullptr;
    }

    esp_matter_bridge::device_t *matter_dev = esp_matter_bridge::resume_device(
        node_,
        endpoint_id,
        nullptr
    );

    if (!matter_dev) {
        ESP_LOGE(TAG, "Failed to resume endpoint %u for '%s'",
                 endpoint_id, dev.persisted.device_id.c_str());
        return nullptr;
    }

    endpoint::enable(matter_dev->endpoint);
    init_endpoint_cluster_callbacks(matter_dev->endpoint);

    // Re-set the label in case it wasn't persisted
    set_endpoint_label(matter_dev->endpoint, dev.persisted.device_id.c_str(), label_suffix);

    ESP_LOGI(TAG, "Resumed endpoint %u for '%s'", endpoint_id, dev.persisted.device_id.c_str());
    return matter_dev;
}

void BridgeState::resume_endpoints_for_device(BridgeDevice &dev)
{
    ESP_LOGI(TAG, "Resuming device '%s' (plug=%u, temp=%u, humidity=%u)",
             dev.persisted.device_id.c_str(),
             dev.persisted.plug_endpoint_id,
             dev.persisted.temp_endpoint_id,
             dev.persisted.humidity_endpoint_id);

    dev.plug_device = resume_single_endpoint(dev, dev.persisted.plug_endpoint_id, "Plug");
    dev.temp_device = resume_single_endpoint(dev, dev.persisted.temp_endpoint_id, "Temp");
    dev.humidity_device = resume_single_endpoint(dev, dev.persisted.humidity_endpoint_id, "Humidity");
}

void BridgeState::create_endpoints_for_device(BridgeDevice &dev, const thread_comms_report_t *report)
{
    ESP_LOGI(TAG, "Creating endpoints for device '%s'", dev.persisted.device_id.c_str());

    // Create plug endpoint if device has relay
    if (report->has_relay_state && dev.persisted.plug_endpoint_id == 0) {
        dev.plug_device = create_single_endpoint(dev, ESP_MATTER_ON_OFF_PLUG_IN_UNIT_DEVICE_TYPE_ID, "Plug");
        if (dev.plug_device) {
            dev.persisted.plug_endpoint_id = dev.plug_device->persistent_info.device_endpoint_id;
        }
    }

    // Create temperature sensor endpoint if device has temperature
    if (report->has_temperature && dev.persisted.temp_endpoint_id == 0) {
        dev.temp_device = create_single_endpoint(dev, ESP_MATTER_TEMPERATURE_SENSOR_DEVICE_TYPE_ID, "Temp");
        if (dev.temp_device) {
            dev.persisted.temp_endpoint_id = dev.temp_device->persistent_info.device_endpoint_id;
        }
    }

    // Create humidity sensor endpoint if device has humidity
    if (report->has_humidity && dev.persisted.humidity_endpoint_id == 0) {
        dev.humidity_device = create_single_endpoint(dev, ESP_MATTER_HUMIDITY_SENSOR_DEVICE_TYPE_ID, "Humidity");
        if (dev.humidity_device) {
            dev.persisted.humidity_endpoint_id = dev.humidity_device->persistent_info.device_endpoint_id;
        }
    }
}

void BridgeState::on_report(const thread_comms_report_t *report)
{
    BridgeDevice *dev = find_by_device_id(report->device_id);

    if (!dev) {
        // New device
        BridgeDevice new_dev;
        new_dev.persisted.device_id = report->device_id;

        // Populate sensor values before creating endpoints
        if (report->has_temperature) {
            new_dev.persisted.temperature = report->temperature;
        }
        if (report->has_humidity) {
            new_dev.persisted.humidity = report->humidity;
        }
        if (report->has_relay_state) {
            new_dev.persisted.relay_state = report->relay_state;
        }

        // Create Matter endpoints for each capability
        create_endpoints_for_device(new_dev, report);

        devices_.push_back(std::move(new_dev));
        dev = &devices_.back();
    } else {
        // Existing device - create any missing endpoints (for migration or new capabilities)
        create_endpoints_for_device(*dev, report);
    }

    // Update sensor values
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

    // Send any pending command
    // So it creates a discrepancy in what matter's world view is vs what the thread device state will be
    if (dev->cmd_pending) {
        send_pending_command(*dev);
        dev->cmd_pending = false;
    }
    // We dont want to update matter attributes if there is a command pending,
    // This is because the command will likely change the state. 
    else {
        // Update Matter attributes
        updating_from_thread = true;
        update_matter_attributes(*dev);
        updating_from_thread = false;

    }
}

void BridgeState::update_matter_attributes(BridgeDevice &dev)
{
    // Update temperature on temp sensor endpoint
    if (dev.persisted.temperature.has_value() && dev.temp_device && dev.temp_device->endpoint) {
        uint16_t ep_id = endpoint::get_id(dev.temp_device->endpoint);
        int16_t temp_val = static_cast<int16_t>(dev.persisted.temperature.value() * 100);
        esp_matter_attr_val_t val = esp_matter_nullable_int16(temp_val);
        attribute::update(ep_id, chip::app::Clusters::TemperatureMeasurement::Id,
                          chip::app::Clusters::TemperatureMeasurement::Attributes::MeasuredValue::Id, &val);
        ESP_LOGI(TAG, "Updated temperature on endpoint %u: %.1fC", ep_id, dev.persisted.temperature.value());
    }

    // Update humidity on humidity sensor endpoint
    if (dev.persisted.humidity.has_value() && dev.humidity_device && dev.humidity_device->endpoint) {
        uint16_t ep_id = endpoint::get_id(dev.humidity_device->endpoint);
        uint16_t humidity_val = static_cast<uint16_t>(dev.persisted.humidity.value() * 100);
        esp_matter_attr_val_t val = esp_matter_nullable_uint16(humidity_val);
        attribute::update(ep_id, chip::app::Clusters::RelativeHumidityMeasurement::Id,
                          chip::app::Clusters::RelativeHumidityMeasurement::Attributes::MeasuredValue::Id, &val);
        ESP_LOGI(TAG, "Updated humidity on endpoint %u: %.1f%%", ep_id, dev.persisted.humidity.value());
    }

    // Update relay state on plug endpoint
    if (dev.persisted.relay_state.has_value() && dev.plug_device && dev.plug_device->endpoint) {
        uint16_t ep_id = endpoint::get_id(dev.plug_device->endpoint);
        esp_matter_attr_val_t val = esp_matter_bool(dev.persisted.relay_state.value());
        attribute::update(ep_id, chip::app::Clusters::OnOff::Id,
                          chip::app::Clusters::OnOff::Attributes::OnOff::Id, &val);
        ESP_LOGI(TAG, "Updated relay on endpoint %u: %s", ep_id, dev.persisted.relay_state.value() ? "ON" : "OFF");
    }
}

void BridgeState::queue_cmd(uint16_t endpoint_id, bool relay_state)
{
    BridgeDevice *dev = find_by_plug_endpoint(endpoint_id);
    if (!dev) {
        ESP_LOGW(TAG, "queue_cmd: plug endpoint %u not found", endpoint_id);
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

BridgeDevice *BridgeState::find_by_plug_endpoint(uint16_t endpoint_id)
{
    for (auto &dev : devices_) {
        if (dev.persisted.plug_endpoint_id == endpoint_id) {
            return &dev;
        }
    }
    return nullptr;
}
