#include "comms.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
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
#include "pb_decode.h"
#include "proto/messages.pb.h"

static const char *TAG = "comms";

/* Company ID for development use */
#define COMMS_COMPANY_ID 0xFFFF

/* Use single advertising channel for deterministic communication */
#define COMMS_ADV_CHANNEL_MAP   0x01  /* Channel 37 only */
#define COMMS_ADV_MAX_EVENTS    1     /* Single advertisement */

/*── State ──*/

static char g_device_id[32];
static comms_source_t g_source;
static uint8_t g_own_addr_type;
static volatile bool g_synced = false;

/* Pre-encoded Hello message */
static uint8_t g_hello_data[Hello_size + 4];
static size_t g_hello_data_len;

/* Scanning callback */
static comms_hello_cb_t g_hello_callback = NULL;

/* Advertising completion semaphore */
static SemaphoreHandle_t g_adv_complete_sem = NULL;

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

/**
 * Parse manufacturer data looking for Hello message.
 * Returns true if Hello was found and decoded.
 */
static bool parse_hello_from_mfg_data(const uint8_t *data, uint8_t len, Hello *hello) {
    /* Manufacturer data format: [company_id_lo][company_id_hi][protobuf...] */
    if (len < 3) {
        return false;
    }

    uint16_t company_id = data[0] | (data[1] << 8);
    if (company_id != COMMS_COMPANY_ID) {
        return false;
    }

    /* Decode protobuf */
    pb_istream_t stream = pb_istream_from_buffer(data + 2, len - 2);
    if (!pb_decode(&stream, Hello_fields, hello)) {
        ESP_LOGD(TAG, "Failed to decode Hello: %s", PB_GET_ERROR(&stream));
        return false;
    }

    return true;
}

/**
 * GAP event handler for advertising.
 */
static int adv_event_handler(struct ble_gap_event *event, void *arg) {
    if (event->type == BLE_GAP_EVENT_ADV_COMPLETE) {
        if (g_adv_complete_sem != NULL) {
            xSemaphoreGive(g_adv_complete_sem);
        }
    }
    return 0;
}

/**
 * Convert internal Hello to public comms_hello_t and invoke callback.
 */
static void invoke_hello_callback(const Hello *internal) {
    if (g_hello_callback == NULL) {
        return;
    }

    comms_hello_t hello = {0};
    hello.source = (internal->source_type == SourceType_SOURCE_TYPE_HUB)
        ? COMMS_SOURCE_HUB
        : COMMS_SOURCE_NODE;
    strncpy(hello.device_id, internal->device_id, sizeof(hello.device_id) - 1);

    g_hello_callback(&hello);
}

static int gap_event_handler(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
        case BLE_GAP_EVENT_EXT_DISC: {
            /* Extended advertisement received */
            struct ble_gap_ext_disc_desc *desc = &event->ext_disc;

            /* Look for manufacturer data in the advertisement */
            struct ble_hs_adv_fields fields;
            if (ble_hs_adv_parse_fields(&fields, desc->data, desc->length_data) != 0) {
                break;
            }

            if (fields.mfg_data != NULL && fields.mfg_data_len > 0) {
                Hello hello = Hello_init_zero;
                if (parse_hello_from_mfg_data(fields.mfg_data, fields.mfg_data_len, &hello)) {
                    invoke_hello_callback(&hello);
                }
            }
            break;
        }

        case BLE_GAP_EVENT_DISC: {
            /* Legacy advertisement received */
            struct ble_gap_disc_desc *desc = &event->disc;

            struct ble_hs_adv_fields fields;
            if (ble_hs_adv_parse_fields(&fields, desc->data, desc->length_data) != 0) {
                break;
            }

            if (fields.mfg_data != NULL && fields.mfg_data_len > 0) {
                Hello hello = Hello_init_zero;
                if (parse_hello_from_mfg_data(fields.mfg_data, fields.mfg_data_len, &hello)) {
                    invoke_hello_callback(&hello);
                }
            }
            break;
        }

        case BLE_GAP_EVENT_DISC_COMPLETE:
            ESP_LOGI(TAG, "Scan complete");
            break;

        default:
            break;
    }
    return 0;
}

/*── Public API ──*/

esp_err_t comms_init(const char *device_id, comms_source_t source) {
    strncpy(g_device_id, device_id, sizeof(g_device_id) - 1);
    g_source = source;

    /* Pre-encode Hello message (convert public type to internal) */
    Hello hello = Hello_init_zero;
    hello.source_type = (source == COMMS_SOURCE_HUB)
        ? SourceType_SOURCE_TYPE_HUB
        : SourceType_SOURCE_TYPE_NODE;
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

    const char *type_str = (source == COMMS_SOURCE_HUB) ? "hub" : "node";
    ESP_LOGI(TAG, "Comms initialized as '%s' (%s, Hello: %d bytes)",
             device_id, type_str, (int)stream.bytes_written);
    return ESP_OK;
}

void comms_deinit(void) {
    g_device_id[0] = '\0';
    g_hello_data_len = 0;
    g_hello_callback = NULL;
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
    comms_stop_scanning();
    ble_gap_ext_adv_stop(0);
    nimble_port_stop();
    nimble_port_deinit();
    g_synced = false;
    ESP_LOGI(TAG, "Comms closed");
}

esp_err_t comms_send_hello(void) {
    int rc;
    static uint8_t ext_adv_instance = 0;

    /* Create semaphore if needed */
    if (g_adv_complete_sem == NULL) {
        g_adv_complete_sem = xSemaphoreCreateBinary();
        if (g_adv_complete_sem == NULL) {
            ESP_LOGE(TAG, "Failed to create semaphore");
            return ESP_FAIL;
        }
    }

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
    ext_params.channel_map = COMMS_ADV_CHANNEL_MAP;
    ext_params.tx_power = 127;

    rc = ble_gap_ext_adv_configure(ext_adv_instance, &ext_params, NULL,
                                   adv_event_handler, NULL);
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

    /* Start advertising with max_events, then wait for completion */
    rc = ble_gap_ext_adv_start(ext_adv_instance, 0, COMMS_ADV_MAX_EVENTS);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_ext_adv_start failed: %d", rc);
        return ESP_FAIL;
    }

    /* Block until advertising completes */
    if (xSemaphoreTake(g_adv_complete_sem, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGW(TAG, "Advertising completion timeout");
        ble_gap_ext_adv_stop(ext_adv_instance);
    }

    ESP_LOGI(TAG, "Hello sent");
    return ESP_OK;
}

esp_err_t comms_send_hello_for(uint32_t duration_ms) {
    int rc;
    static uint8_t ext_adv_instance = 0;

    /* Create semaphore if needed */
    if (g_adv_complete_sem == NULL) {
        g_adv_complete_sem = xSemaphoreCreateBinary();
        if (g_adv_complete_sem == NULL) {
            ESP_LOGE(TAG, "Failed to create semaphore");
            return ESP_FAIL;
        }
    }

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
    ext_params.channel_map = COMMS_ADV_CHANNEL_MAP;
    ext_params.tx_power = 127;

    rc = ble_gap_ext_adv_configure(ext_adv_instance, &ext_params, NULL,
                                   adv_event_handler, NULL);
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

    /* Start advertising with duration (in 10ms units) */
    uint16_t duration_10ms = duration_ms / 10;
    rc = ble_gap_ext_adv_start(ext_adv_instance, duration_10ms, 0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_ext_adv_start failed: %d", rc);
        return ESP_FAIL;
    }

    /* Block until advertising completes */
    if (xSemaphoreTake(g_adv_complete_sem, pdMS_TO_TICKS(duration_ms + 1000)) != pdTRUE) {
        ESP_LOGW(TAG, "Advertising completion timeout");
        ble_gap_ext_adv_stop(ext_adv_instance);
    }

    ESP_LOGI(TAG, "Hello broadcast complete (%lu ms)", (unsigned long)duration_ms);
    return ESP_OK;
}

esp_err_t comms_start_scanning(comms_hello_cb_t callback) {
    g_hello_callback = callback;

    struct ble_gap_ext_disc_params disc_params = {0};
    disc_params.itvl = 160;      /* 100ms */
    disc_params.window = 80;     /* 50ms */
    disc_params.passive = 1;     /* Passive scan (no scan requests) */

    int rc = ble_gap_ext_disc(g_own_addr_type,
                              0,        /* duration: forever */
                              0,        /* period: no periodic */
                              0,        /* filter_duplicates: disabled */
                              0,        /* filter_policy: no whitelist */
                              0,        /* limited: no */
                              &disc_params,  /* 1M PHY params */
                              NULL,          /* coded PHY params: NULL (not using coded PHY) */
                              gap_event_handler,
                              NULL);

    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_ext_disc failed: %d", rc);
        g_hello_callback = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Scanning started");
    return ESP_OK;
}

void comms_stop_scanning(void) {
    ble_gap_disc_cancel();
    g_hello_callback = NULL;
    ESP_LOGI(TAG, "Scanning stopped");
}
