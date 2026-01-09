#include "thread_comms.h"
#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include <string.h>

#include "openthread/instance.h"
#include "openthread/ip6.h"
#include "openthread/udp.h"

/* Nanopb */
#include "pb_encode.h"
#include "pb_decode.h"
#include "messages.pb.h"

static const char *TAG = "thread_comms";

#define THREAD_COMMS_PORT 5683
#define THREAD_COMMS_MULTICAST_ADDR "ff03::1"

/*── State ──*/

static char g_device_id[32];
static thread_comms_source_t g_source;
static otUdpSocket g_socket;
static bool g_socket_open = false;
static thread_comms_callback_t g_callback = NULL;

/*── Internal ──*/

/**
 * Handle received UDP message
 */
static void handle_receive(void *context, otMessage *message, const otMessageInfo *info)
{
    (void)context;

    uint16_t len = otMessageGetLength(message) - otMessageGetOffset(message);
    if (len > Message_size + 16) {
        ESP_LOGW(TAG, "Message too large: %d bytes", len);
        return;
    }

    uint8_t buffer[Message_size + 16];
    uint16_t read = otMessageRead(message, otMessageGetOffset(message), buffer, len);
    if (read != len) {
        ESP_LOGW(TAG, "Failed to read message data");
        return;
    }

    /* Decode protobuf Message */
    Message msg = Message_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(buffer, len);
    if (!pb_decode(&stream, Message_fields, &msg)) {
        ESP_LOGW(TAG, "Failed to decode message: %s", PB_GET_ERROR(&stream));
        return;
    }

    /* Convert to thread_comms_message_t and invoke callback */
    if (g_callback == NULL) {
        return;
    }

    thread_comms_message_t out;
    memset(&out, 0, sizeof(out));

    if (msg.which_payload == Message_report_tag) {
        out.type = THREAD_COMMS_MSG_REPORT;
        strncpy(out.report.device_id, msg.payload.report.device_id, sizeof(out.report.device_id) - 1);
        out.report.has_temperature = msg.payload.report.has_temperature;
        out.report.temperature = msg.payload.report.temperature;
        out.report.has_humidity = msg.payload.report.has_humidity;
        out.report.humidity = msg.payload.report.humidity;
        out.report.has_relay_state = msg.payload.report.has_relay_state;
        out.report.relay_state = msg.payload.report.relay_state;
    } else if (msg.which_payload == Message_relay_cmd_tag) {
        out.type = THREAD_COMMS_MSG_RELAY_CMD;
        strncpy(out.relay_cmd.device_id, msg.payload.relay_cmd.device_id, sizeof(out.relay_cmd.device_id) - 1);
        out.relay_cmd.relay_state = msg.payload.relay_cmd.relay_state;
    } else {
        ESP_LOGW(TAG, "Unknown message payload type");
        return;
    }

    g_callback(&out);
}

/**
 * Send raw protobuf message via UDP multicast
 */
static esp_err_t send_message(const Message *msg)
{
    otInstance *instance = esp_openthread_get_instance();
    if (instance == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Encode message */
    uint8_t buffer[Message_size];
    pb_ostream_t stream = pb_ostream_from_buffer(buffer, sizeof(buffer));
    if (!pb_encode(&stream, Message_fields, msg)) {
        ESP_LOGE(TAG, "Failed to encode message: %s", PB_GET_ERROR(&stream));
        return ESP_FAIL;
    }

    /* Create OpenThread message */
    otMessage *ot_msg = otUdpNewMessage(instance, NULL);
    if (ot_msg == NULL) {
        ESP_LOGE(TAG, "Failed to allocate OT message");
        return ESP_ERR_NO_MEM;
    }

    otError err = otMessageAppend(ot_msg, buffer, stream.bytes_written);
    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "Failed to append message data: %d", err);
        otMessageFree(ot_msg);
        return ESP_FAIL;
    }

    /* Set destination */
    otMessageInfo info;
    memset(&info, 0, sizeof(info));
    otIp6AddressFromString(THREAD_COMMS_MULTICAST_ADDR, &info.mPeerAddr);
    info.mPeerPort = THREAD_COMMS_PORT;

    /* Send */
    esp_openthread_lock_acquire(portMAX_DELAY);
    err = otUdpSend(instance, &g_socket, ot_msg, &info);
    esp_openthread_lock_release();

    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "Failed to send UDP message: %d", err);
        /* Message is freed by otUdpSend on failure */
        return ESP_FAIL;
    }

    return ESP_OK;
}

/*── Public API ──*/

esp_err_t thread_comms_init(const char *device_id, thread_comms_source_t source)
{
    strncpy(g_device_id, device_id, sizeof(g_device_id) - 1);
    g_device_id[sizeof(g_device_id) - 1] = '\0';
    g_source = source;

    const char *type_str = (source == THREAD_COMMS_SOURCE_ROUTER) ? "router" : "end-device";
    ESP_LOGI(TAG, "Thread comms initialized as '%s' (%s)", device_id, type_str);
    return ESP_OK;
}

void thread_comms_deinit(void)
{
    thread_comms_stop();
    g_device_id[0] = '\0';
    g_callback = NULL;
}

esp_err_t thread_comms_start(void)
{
    if (g_socket_open) {
        return ESP_OK;  /* Already started */
    }

    otInstance *instance = esp_openthread_get_instance();
    if (instance == NULL) {
        ESP_LOGE(TAG, "OpenThread not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    esp_openthread_lock_acquire(portMAX_DELAY);

    /* Open UDP socket */
    memset(&g_socket, 0, sizeof(g_socket));
    otError err = otUdpOpen(instance, &g_socket, handle_receive, NULL);
    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "Failed to open UDP socket: %d", err);
        esp_openthread_lock_release();
        return ESP_FAIL;
    }

    /* Bind to port */
    otSockAddr sockaddr;
    memset(&sockaddr, 0, sizeof(sockaddr));
    sockaddr.mPort = THREAD_COMMS_PORT;
    err = otUdpBind(instance, &g_socket, &sockaddr, OT_NETIF_THREAD_HOST);
    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "Failed to bind UDP socket: %d", err);
        otUdpClose(instance, &g_socket);
        esp_openthread_lock_release();
        return ESP_FAIL;
    }

    /* Subscribe to multicast address */
    otIp6Address multicast_addr;
    otIp6AddressFromString(THREAD_COMMS_MULTICAST_ADDR, &multicast_addr);
    err = otIp6SubscribeMulticastAddress(instance, &multicast_addr);
    if (err != OT_ERROR_NONE && err != OT_ERROR_ALREADY) {
        ESP_LOGW(TAG, "Failed to subscribe to multicast: %d (may already be subscribed)", err);
        /* Continue anyway - might already be subscribed */
    }

    esp_openthread_lock_release();

    g_socket_open = true;
    ESP_LOGI(TAG, "Thread comms started (port %d, multicast %s)", THREAD_COMMS_PORT, THREAD_COMMS_MULTICAST_ADDR);
    return ESP_OK;
}

void thread_comms_stop(void)
{
    if (!g_socket_open) {
        return;
    }

    otInstance *instance = esp_openthread_get_instance();
    if (instance != NULL) {
        esp_openthread_lock_acquire(portMAX_DELAY);
        otUdpClose(instance, &g_socket);
        esp_openthread_lock_release();
    }

    g_socket_open = false;
    ESP_LOGI(TAG, "Thread comms stopped");
}

esp_err_t thread_comms_send_report(const thread_comms_report_t *report)
{
    if (!g_socket_open) {
        return ESP_ERR_INVALID_STATE;
    }

    Message msg = Message_init_zero;
    msg.which_payload = Message_report_tag;
    strncpy(msg.payload.report.device_id, report->device_id, sizeof(msg.payload.report.device_id) - 1);

    if (report->has_temperature) {
        msg.payload.report.has_temperature = true;
        msg.payload.report.temperature = report->temperature;
    }
    if (report->has_humidity) {
        msg.payload.report.has_humidity = true;
        msg.payload.report.humidity = report->humidity;
    }
    if (report->has_relay_state) {
        msg.payload.report.has_relay_state = true;
        msg.payload.report.relay_state = report->relay_state;
    }

    return send_message(&msg);
}

esp_err_t thread_comms_send_relay_cmd(const thread_comms_relay_cmd_t *cmd)
{
    if (!g_socket_open) {
        return ESP_ERR_INVALID_STATE;
    }

    Message msg = Message_init_zero;
    msg.which_payload = Message_relay_cmd_tag;
    strncpy(msg.payload.relay_cmd.device_id, cmd->device_id, sizeof(msg.payload.relay_cmd.device_id) - 1);
    msg.payload.relay_cmd.relay_state = cmd->relay_state;

    return send_message(&msg);
}

void thread_comms_set_callback(thread_comms_callback_t callback)
{
    g_callback = callback;
}
