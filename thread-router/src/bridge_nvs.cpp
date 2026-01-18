#include "bridge_nvs.hpp"

#include <cstring>

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include <pb_encode.h>
#include <pb_decode.h>
#include "proto/bridge_nvs.pb.h"

static const char *TAG = "tr-nvs";

static const char *NVS_NAMESPACE = "bridge";
static const char *KEY_GLOBAL = "tr-global";
static const char *KEY_DEVICE_PREFIX = "tr-dev-";

static nvs_handle_t s_nvs_handle = 0;

esp_err_t bridge_nvs_init()
{
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s': %s", NVS_NAMESPACE, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "NVS initialized");
    return ESP_OK;
}

uint16_t bridge_nvs_get_next_endpoint_id()
{
    uint8_t buf[BridgeNvsGlobal_size];
    size_t len = sizeof(buf);

    esp_err_t err = nvs_get_blob(s_nvs_handle, KEY_GLOBAL, buf, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // First time - start at endpoint 1 (0 is reserved for root node)
        return 1;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read global: %s", esp_err_to_name(err));
        return 1;
    }

    BridgeNvsGlobal global = BridgeNvsGlobal_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(buf, len);
    if (!pb_decode(&stream, BridgeNvsGlobal_fields, &global)) {
        ESP_LOGE(TAG, "Failed to decode global");
        return 1;
    }

    return static_cast<uint16_t>(global.next_endpoint_id);
}

uint16_t bridge_nvs_alloc_endpoint_id()
{
    uint16_t id = bridge_nvs_get_next_endpoint_id();

    // Increment and save
    BridgeNvsGlobal global = BridgeNvsGlobal_init_zero;
    global.next_endpoint_id = id + 1;

    uint8_t buf[BridgeNvsGlobal_size];
    pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&stream, BridgeNvsGlobal_fields, &global)) {
        ESP_LOGE(TAG, "Failed to encode global");
        return id;
    }

    esp_err_t err = nvs_set_blob(s_nvs_handle, KEY_GLOBAL, buf, stream.bytes_written);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write global: %s", esp_err_to_name(err));
        return id;
    }

    err = nvs_commit(s_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Allocated endpoint ID: %u", id);
    return id;
}

const char *bridge_nvs_get_hex_suffix(const char *device_id)
{
    if (!device_id) return nullptr;

    // Find last '-' in "vivid-falcon-a3f2"
    const char *last_dash = strrchr(device_id, '-');
    if (!last_dash || strlen(last_dash + 1) != 4) {
        return nullptr;
    }
    return last_dash + 1;
}

static void make_device_key(char *key_buf, size_t key_buf_size, const char *hex_suffix)
{
    snprintf(key_buf, key_buf_size, "%s%s", KEY_DEVICE_PREFIX, hex_suffix);
}

esp_err_t bridge_nvs_save_device(const BridgeDeviceState &device)
{
    const char *hex = bridge_nvs_get_hex_suffix(device.device_id.c_str());
    if (!hex) {
        ESP_LOGE(TAG, "Invalid device_id format: %s", device.device_id.c_str());
        return ESP_ERR_INVALID_ARG;
    }

    char key[16];
    make_device_key(key, sizeof(key), hex);

    // Convert to nanopb struct
    BridgeNvsDevice pb_device = BridgeNvsDevice_init_zero;
    strncpy(pb_device.device_id, device.device_id.c_str(), sizeof(pb_device.device_id) - 1);
    pb_device.endpoint_id = device.endpoint_id;

    if (device.temperature.has_value()) {
        pb_device.has_temperature = true;
        pb_device.temperature = device.temperature.value();
    }
    if (device.humidity.has_value()) {
        pb_device.has_humidity = true;
        pb_device.humidity = device.humidity.value();
    }
    if (device.relay_state.has_value()) {
        pb_device.has_relay_state = true;
        pb_device.relay_state = device.relay_state.value();
    }

    // Encode
    uint8_t buf[BridgeNvsDevice_size];
    pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
    if (!pb_encode(&stream, BridgeNvsDevice_fields, &pb_device)) {
        ESP_LOGE(TAG, "Failed to encode device");
        return ESP_FAIL;
    }

    // Write to NVS
    esp_err_t err = nvs_set_blob(s_nvs_handle, key, buf, stream.bytes_written);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write device %s: %s", key, esp_err_to_name(err));
        return err;
    }

    err = nvs_commit(s_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Saved device: %s (endpoint %u)", device.device_id.c_str(), device.endpoint_id);
    return ESP_OK;
}

std::optional<BridgeDeviceState> bridge_nvs_load_device(const char *hex_suffix)
{
    char key[16];
    make_device_key(key, sizeof(key), hex_suffix);

    uint8_t buf[BridgeNvsDevice_size];
    size_t len = sizeof(buf);

    esp_err_t err = nvs_get_blob(s_nvs_handle, key, buf, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return std::nullopt;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read device %s: %s", key, esp_err_to_name(err));
        return std::nullopt;
    }

    BridgeNvsDevice pb_device = BridgeNvsDevice_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(buf, len);
    if (!pb_decode(&stream, BridgeNvsDevice_fields, &pb_device)) {
        ESP_LOGE(TAG, "Failed to decode device %s", key);
        return std::nullopt;
    }

    // Convert to C++ struct
    BridgeDeviceState device;
    device.device_id = pb_device.device_id;
    device.endpoint_id = static_cast<uint16_t>(pb_device.endpoint_id);

    if (pb_device.has_temperature) {
        device.temperature = pb_device.temperature;
    }
    if (pb_device.has_humidity) {
        device.humidity = pb_device.humidity;
    }
    if (pb_device.has_relay_state) {
        device.relay_state = pb_device.relay_state;
    }

    return device;
}

esp_err_t bridge_nvs_delete_device(const char *hex_suffix)
{
    char key[16];
    make_device_key(key, sizeof(key), hex_suffix);

    esp_err_t err = nvs_erase_key(s_nvs_handle, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;  // Already gone
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to delete device %s: %s", key, esp_err_to_name(err));
        return err;
    }

    err = nvs_commit(s_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Deleted device: %s", hex_suffix);
    return ESP_OK;
}

std::vector<BridgeDeviceState> bridge_nvs_load_all_devices()
{
    std::vector<BridgeDeviceState> devices;

    nvs_iterator_t it = nullptr;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, NVS_NAMESPACE, NVS_TYPE_BLOB, &it);

    while (err == ESP_OK && it != nullptr) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);

        // Check if this is a device key (starts with "tr-dev-")
        if (strncmp(info.key, KEY_DEVICE_PREFIX, strlen(KEY_DEVICE_PREFIX)) == 0) {
            const char *hex_suffix = info.key + strlen(KEY_DEVICE_PREFIX);
            auto device = bridge_nvs_load_device(hex_suffix);
            if (device.has_value()) {
                devices.push_back(std::move(device.value()));
            }
        }

        err = nvs_entry_next(&it);
    }

    nvs_release_iterator(it);

    ESP_LOGI(TAG, "Loaded %zu devices from NVS", devices.size());
    return devices;
}

esp_err_t bridge_nvs_erase_all()
{
    esp_err_t err = nvs_erase_all(s_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase all: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_commit(s_nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGW(TAG, "Erased all bridge data from NVS");
    return ESP_OK;
}
