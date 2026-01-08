#include "comms.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
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

/* Magic number to identify our messages */
#define COMMS_MAGIC 0x48, 0x41  /* "HA" */

/* Use single advertising channel to reduce power and simplify scanning */
#define COMMS_ADV_CHANNEL_MAP   0x04  /* Channel 39 only */

/* Internal scan buffer size */
#define COMMS_SCAN_BUFFER_SIZE  16

/*── State ──*/

static char g_device_id[32];
static comms_source_t g_source;
static uint8_t g_own_addr_type;
static volatile bool g_synced = false;

/* Generate unique message ID: upper 16 bits = time (ms), lower 16 bits = random */
static uint32_t generate_message_id(void) {
    uint32_t time_ms = (uint32_t)(esp_timer_get_time() / 1000);
    return ((time_ms & 0xFFFF) << 16) | (esp_random() & 0xFFFF);
}

/* Advertising completion semaphore */
static SemaphoreHandle_t g_adv_complete_sem = NULL;

/* Scan buffer (for comms_scan_for) */
static comms_message_t *g_scan_buffer = NULL;
static int g_scan_buffer_max = 0;
static int g_scan_buffer_count = 0;

/* Callback (for comms_start_scanning) */
static comms_message_cb_t g_message_callback = NULL;

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
 * Convert protobuf Message to comms_message_t.
 */
static void convert_message(const Message *msg, const char *device_id, comms_message_t *out) {
    memset(out, 0, sizeof(*out));
    strncpy(out->device_id, device_id, sizeof(out->device_id) - 1);
    out->message_id = msg->message_id;

    switch (msg->type) {
        case MessageType_MESSAGE_TYPE_HELLO:
            if (msg->which_payload == Message_hello_tag) {
                out->has_hello = true;
                out->hello.source = (msg->payload.hello.source_type == SourceType_SOURCE_TYPE_HUB)
                    ? COMMS_SOURCE_HUB
                    : COMMS_SOURCE_NODE;
            }
            break;

        case MessageType_MESSAGE_TYPE_REPORT:
            if (msg->which_payload == Message_report_tag) {
                const SensorReport *r = &msg->payload.report;
                out->has_report = true;
                if (r->has_temperature_c) {
                    out->report.has_temperature = true;
                    out->report.temperature_c = r->temperature_c;
                }
                if (r->has_humidity_pct) {
                    out->report.has_humidity = true;
                    out->report.humidity_pct = r->humidity_pct;
                }
                if (r->has_relay_state) {
                    out->report.has_relay = true;
                    out->report.relay_state = r->relay_state;
                }
            }
            break;

        case MessageType_MESSAGE_TYPE_RELAY_COMMAND:
            if (msg->which_payload == Message_relay_cmd_tag) {
                const RelayCommand *cmd = &msg->payload.relay_cmd;
                out->has_relay_cmd = true;
                strncpy(out->relay_cmd.device_id, cmd->device_id, sizeof(out->relay_cmd.device_id) - 1);
                out->relay_cmd.relay_id = cmd->relay_id;
                out->relay_cmd.state = cmd->state;
            }
            break;

        default:
            break;
    }
}

/**
 * Handle received message - either callback or store in buffer.
 */
static void handle_message(const Message *msg, const char *device_id) {
    /* Callback mode: deliver immediately */
    if (g_message_callback != NULL) {
        comms_message_t out;
        convert_message(msg, device_id, &out);
        g_message_callback(&out);
        return;
    }

    /* Buffer mode: store with deduping */
    if (g_scan_buffer != NULL) {
        /* Check for duplicate message_id */
        for (int i = 0; i < g_scan_buffer_count; i++) {
            if (g_scan_buffer[i].message_id == msg->message_id) {
                return;  /* Already have this message */
            }
        }

        if (g_scan_buffer_count >= g_scan_buffer_max) {
            ESP_LOGW(TAG, "Scan buffer full, dropping message from %s", device_id);
            return;
        }

        convert_message(msg, device_id, &g_scan_buffer[g_scan_buffer_count++]);
    }
}

/**
 * Find manufacturer data in raw advertisement LTV data.
 * Returns pointer to mfg data (after type byte) and sets len, or NULL if not found.
 *
 * Why manual parsing? NimBLE's ble_hs_adv_parse_fields() was designed for legacy
 * advertising (31 bytes max) and rejects any single field >29 bytes with BLE_HS_EBADDATA.
 * Our SensorReport messages exceed this limit, so we parse LTV fields manually.
 * See: nimble/host/src/ble_hs_adv.c line ~692
 */
static const uint8_t *find_mfg_data(const uint8_t *data, uint8_t data_len, uint8_t *out_len) {
    while (data_len >= 2) {
        uint8_t field_len = data[0];
        uint8_t field_type = data[1];

        if (field_len == 0 || field_len > data_len - 1) {
            break;  /* Invalid or truncated */
        }

        if (field_type == BLE_HS_ADV_TYPE_MFG_DATA && field_len >= 3) {
            *out_len = field_len - 1;  /* Exclude type byte */
            return data + 2;           /* Point to mfg data content */
        }

        data += field_len + 1;
        data_len -= field_len + 1;
    }
    return NULL;
}

/**
 * Parse manufacturer data looking for Message.
 */
static bool parse_message_from_mfg_data(const uint8_t *data, uint8_t len) {
    /* Manufacturer data format: [company_id_lo][company_id_hi][magic_hi][magic_lo][protobuf...] */
    static const uint8_t magic[] = { COMMS_MAGIC };

    if (len < 4) {
        return false;
    }

    uint16_t company_id = data[0] | (data[1] << 8);
    if (company_id != COMMS_COMPANY_ID) {
        return false;
    }

    if (data[2] != magic[0] || data[3] != magic[1]) {
        return false;  /* Not our message */
    }

    /* Decode protobuf Message wrapper */
    Message msg = Message_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(data + 4, len - 4);
    if (!pb_decode(&stream, Message_fields, &msg)) {
        ESP_LOGW(TAG, "Failed to decode Message: %s", PB_GET_ERROR(&stream));
        return false;
    }

    /* Get device_id from payload */
    const char *device_id = NULL;
    if (msg.which_payload == Message_hello_tag) {
        device_id = msg.payload.hello.device_id;
    } else if (msg.which_payload == Message_report_tag) {
        device_id = msg.payload.report.device_id;
    } else if (msg.which_payload == Message_relay_cmd_tag) {
        device_id = msg.payload.relay_cmd.device_id;
    }

    if (device_id) {
        handle_message(&msg, device_id);
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

static int gap_event_handler(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
        case BLE_GAP_EVENT_EXT_DISC: {
            struct ble_gap_ext_disc_desc *desc = &event->ext_disc;

            /* Use manual LTV parsing for extended advertisements
             * (ble_hs_adv_parse_fields has 29-byte field limit) */
            uint8_t mfg_len;
            const uint8_t *mfg_data = find_mfg_data(desc->data, desc->length_data, &mfg_len);
            if (mfg_data != NULL) {
                parse_message_from_mfg_data(mfg_data, mfg_len);
            }
            break;
        }

        case BLE_GAP_EVENT_DISC: {
            struct ble_gap_disc_desc *desc = &event->disc;
            struct ble_hs_adv_fields fields;
            if (ble_hs_adv_parse_fields(&fields, desc->data, desc->length_data) != 0) {
                break;
            }
            if (fields.mfg_data != NULL && fields.mfg_data_len > 0) {
                parse_message_from_mfg_data(fields.mfg_data, fields.mfg_data_len);
            }
            break;
        }

        case BLE_GAP_EVENT_DISC_COMPLETE:
            ESP_LOGW(TAG, "Scan complete - scanning stopped!");
            break;

        default:
            break;
    }
    return 0;
}

static esp_err_t start_scanning(void) {
    struct ble_gap_ext_disc_params disc_params = {0};
    disc_params.itvl = 160;      /* 100ms */
    disc_params.window = 80;     /* 50ms */
    disc_params.passive = 1;     /* Passive scan */

    int rc = ble_gap_ext_disc(g_own_addr_type,
                              0, 0, 0, 0, 0,
                              &disc_params, NULL,
                              gap_event_handler, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_ext_disc failed: %d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void stop_scanning(void) {
    ble_gap_disc_cancel();
}

static esp_err_t send_message(const Message *msg) {
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

    /* Encode message */
    static const uint8_t magic[] = { COMMS_MAGIC };
    uint8_t msg_data[Message_size + 6];
    pb_ostream_t stream = pb_ostream_from_buffer(msg_data + 6, sizeof(msg_data) - 6);
    if (!pb_encode(&stream, Message_fields, msg)) {
        ESP_LOGE(TAG, "Failed to encode message: %s", PB_GET_ERROR(&stream));
        return ESP_FAIL;
    }

    /* Manufacturer data header: [len][type][company_lo][company_hi][magic0][magic1][protobuf...] */
    msg_data[0] = stream.bytes_written + 5;  /* length = company(2) + magic(2) + protobuf */
    msg_data[1] = BLE_HS_ADV_TYPE_MFG_DATA;
    msg_data[2] = 0xFF;  /* Company ID low */
    msg_data[3] = 0xFF;  /* Company ID high */
    msg_data[4] = magic[0];
    msg_data[5] = magic[1];
    size_t msg_data_len = 6 + stream.bytes_written;

    struct ble_gap_ext_adv_params ext_params = {0};
    ext_params.connectable = 0;
    ext_params.scannable = 0;
    ext_params.legacy_pdu = 0;
    ext_params.itvl_min = 160;
    ext_params.itvl_max = 320;
    ext_params.primary_phy = BLE_HCI_LE_PHY_1M;
    ext_params.secondary_phy = BLE_HCI_LE_PHY_1M;  /* Use 1M for compatibility */
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

    /* Manufacturer data */
    memcpy(&adv_data[adv_len], msg_data, msg_data_len);
    adv_len += msg_data_len;

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

    return ESP_OK;
}

static esp_err_t advertise_for(uint32_t duration_ms) {
    uint16_t duration_10ms = duration_ms / 10;
    int rc = ble_gap_ext_adv_start(0, duration_10ms, 0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_ext_adv_start failed: %d", rc);
        return ESP_FAIL;
    }

    if (xSemaphoreTake(g_adv_complete_sem, pdMS_TO_TICKS(duration_ms + 1000)) != pdTRUE) {
        ESP_LOGW(TAG, "Advertising completion timeout");
        ble_gap_ext_adv_stop(0);
    }

    return ESP_OK;
}

/*── Public API ──*/

esp_err_t comms_init(const char *device_id, comms_source_t source) {
    strncpy(g_device_id, device_id, sizeof(g_device_id) - 1);
    g_source = source;

    const char *type_str = (source == COMMS_SOURCE_HUB) ? "hub" : "node";
    ESP_LOGI(TAG, "Comms initialized as '%s' (%s)", device_id, type_str);
    return ESP_OK;
}

void comms_deinit(void) {
    g_device_id[0] = '\0';
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

esp_err_t comms_send_hello_for(uint32_t duration_ms) {
    Message msg = Message_init_zero;
    msg.message_id = generate_message_id();
    msg.type = MessageType_MESSAGE_TYPE_HELLO;
    msg.which_payload = Message_hello_tag;
    msg.payload.hello.source_type = (g_source == COMMS_SOURCE_HUB)
        ? SourceType_SOURCE_TYPE_HUB
        : SourceType_SOURCE_TYPE_NODE;
    strncpy(msg.payload.hello.device_id, g_device_id, sizeof(msg.payload.hello.device_id) - 1);

    esp_err_t ret = send_message(&msg);
    if (ret != ESP_OK) {
        return ret;
    }

    return advertise_for(duration_ms);
}

esp_err_t comms_send_report_for(const comms_report_t *report, uint32_t duration_ms) {
    Message msg = Message_init_zero;
    msg.message_id = generate_message_id();
    msg.type = MessageType_MESSAGE_TYPE_REPORT;
    msg.which_payload = Message_report_tag;
    strncpy(msg.payload.report.device_id, g_device_id, sizeof(msg.payload.report.device_id) - 1);

    if (report->temperature_c) {
        msg.payload.report.has_temperature_c = true;
        msg.payload.report.temperature_c = *report->temperature_c;
    }
    if (report->humidity_pct) {
        msg.payload.report.has_humidity_pct = true;
        msg.payload.report.humidity_pct = *report->humidity_pct;
    }
    if (report->relay_state) {
        msg.payload.report.has_relay_state = true;
        msg.payload.report.relay_state = *report->relay_state;
    }

    esp_err_t ret = send_message(&msg);
    if (ret != ESP_OK) {
        return ret;
    }

    return advertise_for(duration_ms);
}

esp_err_t comms_send_relay_cmd_for(const comms_relay_cmd_t *cmd, uint32_t duration_ms) {
    Message msg = Message_init_zero;
    msg.message_id = generate_message_id();
    msg.type = MessageType_MESSAGE_TYPE_RELAY_COMMAND;
    msg.which_payload = Message_relay_cmd_tag;
    strncpy(msg.payload.relay_cmd.device_id, cmd->device_id, sizeof(msg.payload.relay_cmd.device_id) - 1);
    msg.payload.relay_cmd.relay_id = cmd->relay_id;
    msg.payload.relay_cmd.state = cmd->state;

    esp_err_t ret = send_message(&msg);
    if (ret != ESP_OK) {
        return ret;
    }

    return advertise_for(duration_ms);
}

esp_err_t comms_start_scanning(comms_message_cb_t callback) {
    g_message_callback = callback;

    esp_err_t ret = start_scanning();
    if (ret != ESP_OK) {
        g_message_callback = NULL;
        ESP_LOGE(TAG, "Failed to start scanning");
        return ret;
    }

    return ESP_OK;
}

void comms_stop_scanning(void) {
    stop_scanning();
    g_message_callback = NULL;
}

int comms_scan_for(uint32_t duration_ms, comms_message_t *out, int max_count) {
    /* Setup scan buffer */
    g_scan_buffer = out;
    g_scan_buffer_max = max_count;
    g_scan_buffer_count = 0;

    /* Start scanning */
    if (start_scanning() != ESP_OK) {
        g_scan_buffer = NULL;
        return 0;
    }

    /* Wait for duration */
    vTaskDelay(pdMS_TO_TICKS(duration_ms));

    /* Stop scanning */
    stop_scanning();

    int count = g_scan_buffer_count;
    g_scan_buffer = NULL;
    g_scan_buffer_max = 0;
    g_scan_buffer_count = 0;

    ESP_LOGI(TAG, "Scan complete: %d messages", count);
    return count;
}
