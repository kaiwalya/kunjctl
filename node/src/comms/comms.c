#include "comms.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

/* NimBLE */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "services/gap/ble_svc_gap.h"
#include "os/os_mbuf.h"

/* Nanopb */
#include "pb_encode.h"
#include "messages.pb.h"

static const char *TAG = "comms";

/*── State ──*/

static char g_device_id[32];
static uint8_t g_own_addr_type;
static volatile bool g_synced = false;

/* Pre-encoded Hello message */
static uint8_t g_hello_data[Hello_size + 4];
static size_t g_hello_data_len;

/*── Internal ──*/

static void nimble_host_task(void *arg) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void on_sync(void) {
    int rc = ble_hs_id_infer_auto(0, &g_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to determine address type: %d", rc);
        return;
    }

    uint8_t addr[6];
    ble_hs_id_copy_addr(g_own_addr_type, addr, NULL);
    ESP_LOGI(TAG, "BLE Address: %02X:%02X:%02X:%02X:%02X:%02X",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);

    g_synced = true;
}

/*── Public API ──*/

esp_err_t comms_init(const char *device_id) {
    strncpy(g_device_id, device_id, sizeof(g_device_id) - 1);

    /* Pre-encode Hello message */
    Hello hello = Hello_init_zero;
    strncpy(hello.device_id, device_id, sizeof(hello.device_id) - 1);

    pb_ostream_t stream = pb_ostream_from_buffer(g_hello_data + 4, sizeof(g_hello_data) - 4);
    if (!pb_encode(&stream, Hello_fields, &hello)) {
        ESP_LOGE(TAG, "Failed to encode Hello: %s", PB_GET_ERROR(&stream));
        return ESP_FAIL;
    }

    /* Manufacturer data: [len][type][company_id_lo][company_id_hi][data...] */
    g_hello_data[0] = stream.bytes_written + 3;
    g_hello_data[1] = BLE_HS_ADV_TYPE_MFG_DATA;
    g_hello_data[2] = 0xFF;  /* Company ID 0xFFFF = development */
    g_hello_data[3] = 0xFF;
    g_hello_data_len = 4 + stream.bytes_written;

    ESP_LOGI(TAG, "Comms initialized as '%s' (Hello: %d bytes)", device_id, (int)stream.bytes_written);
    return ESP_OK;
}

void comms_deinit(void) {
    g_device_id[0] = '\0';
    g_hello_data_len = 0;
}

esp_err_t comms_open(void) {
    g_synced = false;

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_svc_gap_device_name_set(g_device_id);

    nimble_port_freertos_init(nimble_host_task);

    /* Wait for sync */
    while (!g_synced) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGI(TAG, "Comms opened");
    return ESP_OK;
}

void comms_close(void) {
    ble_gap_ext_adv_stop(0);
    nimble_port_stop();
    nimble_port_deinit();
    g_synced = false;
    ESP_LOGI(TAG, "Comms closed");
}

esp_err_t comms_send_hello(void) {
    int rc;
    static uint8_t ext_adv_instance = 0;

    struct ble_gap_ext_adv_params ext_params = {0};
    ext_params.connectable = 0;
    ext_params.scannable = 0;
    ext_params.legacy_pdu = 0;
    ext_params.itvl_min = 160;
    ext_params.itvl_max = 320;
    ext_params.primary_phy = BLE_HCI_LE_PHY_1M;
    ext_params.secondary_phy = BLE_HCI_LE_PHY_2M;
    ext_params.own_addr_type = g_own_addr_type;
    ext_params.sid = 0;
    ext_params.channel_map = 0x07;
    ext_params.tx_power = 127;

    rc = ble_gap_ext_adv_configure(ext_adv_instance, &ext_params, NULL, NULL, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_ext_adv_configure failed: %d", rc);
        return ESP_FAIL;
    }

    /* Build advertisement data */
    uint8_t name_len = strlen(g_device_id);
    uint8_t adv_data[64];
    int adv_len = 0;

    /* Flags */
    adv_data[adv_len++] = 2;
    adv_data[adv_len++] = BLE_HS_ADV_TYPE_FLAGS;
    adv_data[adv_len++] = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    /* Complete local name */
    adv_data[adv_len++] = name_len + 1;
    adv_data[adv_len++] = BLE_HS_ADV_TYPE_COMP_NAME;
    memcpy(&adv_data[adv_len], g_device_id, name_len);
    adv_len += name_len;

    /* Manufacturer data (Hello message) */
    memcpy(&adv_data[adv_len], g_hello_data, g_hello_data_len);
    adv_len += g_hello_data_len;

    /* Set advertisement data */
    struct os_mbuf *data = os_msys_get_pkthdr(adv_len, 0);
    if (!data) {
        ESP_LOGE(TAG, "Failed to allocate mbuf");
        return ESP_FAIL;
    }
    os_mbuf_append(data, adv_data, adv_len);

    rc = ble_gap_ext_adv_set_data(ext_adv_instance, data);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_ext_adv_set_data failed: %d", rc);
        return ESP_FAIL;
    }

    rc = ble_gap_ext_adv_start(ext_adv_instance, 0, 0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_ext_adv_start failed: %d", rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Hello sent");
    return ESP_OK;
}
