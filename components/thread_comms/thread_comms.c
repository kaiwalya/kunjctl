#include "thread_comms.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_types.h"

#include "openthread/dataset.h"
#include "openthread/instance.h"
#include "openthread/ip6.h"
#include "openthread/link.h"
#include "openthread/logging.h"
#include "openthread/thread.h"
#include "openthread/udp.h"

/* Nanopb */
#include "pb_encode.h"
#include "pb_decode.h"
#include "messages.pb.h"

static const char *TAG = "thread_comms";

#define THREAD_COMMS_PORT 5683

/*── State ──*/

static char g_device_id[32];
static thread_comms_source_t g_source;
static otUdpSocket g_socket;
static bool g_initialized = false;
static thread_comms_callback_t g_callback = NULL;

/*── Forward declarations ──*/

static void handle_receive(void *context, otMessage *message, const otMessageInfo *info);

/*── Internal ──*/

static uint32_t generate_msg_id(void)
{
    uint32_t time_part = (uint32_t)(esp_timer_get_time() / 1000000) & 0xFFFF;  /* Seconds, lower 16 bits */
    uint32_t rand_part = esp_random() & 0xFFFF;
    return (time_part << 16) | rand_part;
}

static const char *role_to_string(otDeviceRole role)
{
    switch (role) {
        case OT_DEVICE_ROLE_DISABLED: return "Disabled";
        case OT_DEVICE_ROLE_DETACHED: return "Detached";
        case OT_DEVICE_ROLE_CHILD:    return "Child";
        case OT_DEVICE_ROLE_ROUTER:   return "Router";
        case OT_DEVICE_ROLE_LEADER:   return "Leader";
        default:                      return "Unknown";
    }
}

static void ot_state_changed(otChangedFlags flags, void *ctx)
{
    otInstance *instance = esp_openthread_get_instance();

    if (flags & OT_CHANGED_THREAD_ROLE) {
        otDeviceRole role = otThreadGetDeviceRole(instance);
        ESP_LOGI(TAG, "Role changed: %s", role_to_string(role));
    }

    if (g_source == THREAD_COMMS_SOURCE_ROUTER) {
        if (flags & OT_CHANGED_THREAD_CHILD_ADDED) {
            ESP_LOGI(TAG, "Child joined the network");
        }
        if (flags & OT_CHANGED_THREAD_CHILD_REMOVED) {
            ESP_LOGI(TAG, "Child left the network");
        }
    }
}

static void ot_mainloop(void *arg)
{
    esp_openthread_launch_mainloop();
    vTaskDelete(NULL);
}

static void configure_dataset(otInstance *instance)
{
    otOperationalDataset dataset;
    memset(&dataset, 0, sizeof(dataset));

    /* Network name */
    memcpy(dataset.mNetworkName.m8, "HomeAuto", 9);
    dataset.mComponents.mIsNetworkNamePresent = true;

    /* Channel */
    dataset.mChannel = 15;
    dataset.mComponents.mIsChannelPresent = true;

    /* PAN ID */
    dataset.mPanId = 0x1234;
    dataset.mComponents.mIsPanIdPresent = true;

    /* Extended PAN ID (8 bytes) */
    uint8_t ext_pan_id[8] = {0x11, 0x11, 0x11, 0x11, 0x22, 0x22, 0x22, 0x22};
    memcpy(dataset.mExtendedPanId.m8, ext_pan_id, 8);
    dataset.mComponents.mIsExtendedPanIdPresent = true;

    /* Network key (16 bytes) */
    uint8_t network_key[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                               0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    memcpy(dataset.mNetworkKey.m8, network_key, 16);
    dataset.mComponents.mIsNetworkKeyPresent = true;

    /* Mesh-local prefix */
    uint8_t mesh_local_prefix[8] = {0xfd, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    memcpy(dataset.mMeshLocalPrefix.m8, mesh_local_prefix, 8);
    dataset.mComponents.mIsMeshLocalPrefixPresent = true;

    /* Router-only: extra fields needed to form network */
    if (g_source == THREAD_COMMS_SOURCE_ROUTER) {
        /* Active timestamp (required to form network) */
        dataset.mActiveTimestamp.mSeconds = 1;
        dataset.mActiveTimestamp.mTicks = 0;
        dataset.mActiveTimestamp.mAuthoritative = false;
        dataset.mComponents.mIsActiveTimestampPresent = true;

        /* Security policy (allow joining) */
        dataset.mSecurityPolicy.mRotationTime = 672;
        dataset.mSecurityPolicy.mObtainNetworkKeyEnabled = true;
        dataset.mSecurityPolicy.mNativeCommissioningEnabled = true;
        dataset.mSecurityPolicy.mRoutersEnabled = true;
        dataset.mSecurityPolicy.mExternalCommissioningEnabled = true;
        dataset.mComponents.mIsSecurityPolicyPresent = true;

        /* PSKc - Pre-Shared Key for Commissioner (16 bytes) */
        uint8_t pskc[16] = {0x3a, 0xa5, 0x5f, 0x91, 0xca, 0x47, 0xd1, 0xe4,
                            0xe7, 0x1a, 0x08, 0xcb, 0x35, 0xe9, 0x15, 0x91};
        memcpy(dataset.mPskc.m8, pskc, 16);
        dataset.mComponents.mIsPskcPresent = true;
    }

    otDatasetSetActive(instance, &dataset);
}

static void configure_sed_mode(otInstance *instance)
{
    otLinkModeConfig mode = {0};
    mode.mRxOnWhenIdle = false;  /* Sleep between polls */
    mode.mDeviceType = false;    /* MTD (not FTD) */
    mode.mNetworkData = false;   /* Minimal network data */
    otThreadSetLinkMode(instance, mode);

    /* Disable automatic polling - caller will use thread_comms_poll() */
    otLinkSetPollPeriod(instance, 0);

    ESP_LOGI(TAG, "Configured as Sleepy End Device");
}

static esp_err_t wait_for_role(otDeviceRole min_role, const char *wait_msg)
{
    otInstance *instance = esp_openthread_get_instance();

    ESP_LOGI(TAG, "%s", wait_msg);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_openthread_lock_acquire(portMAX_DELAY);
        otDeviceRole role = otThreadGetDeviceRole(instance);
        esp_openthread_lock_release();
        if (role >= min_role) {
            return ESP_OK;
        }
    }
}

static esp_err_t start_udp(void)
{
    otInstance *instance = esp_openthread_get_instance();

    esp_openthread_lock_acquire(portMAX_DELAY);

    /* Open UDP socket with receive handler */
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

    /* Subscribe to Realm-Local All Thread Nodes multicast */
    const otIp6Address *multicast_addr = otThreadGetRealmLocalAllThreadNodesMulticastAddress(instance);
    err = otIp6SubscribeMulticastAddress(instance, multicast_addr);
    if (err != OT_ERROR_NONE && err != OT_ERROR_ALREADY) {
        ESP_LOGW(TAG, "Failed to subscribe to multicast: %d", err);
    }

    char addr_str[40];
    otIp6AddressToString(multicast_addr, addr_str, sizeof(addr_str));
    ESP_LOGI(TAG, "UDP ready (port %d, multicast %s)", THREAD_COMMS_PORT, addr_str);

    esp_openthread_lock_release();
    return ESP_OK;
}

/**
 * Handle received UDP message
 */
static void handle_receive(void *context, otMessage *message, const otMessageInfo *info)
{
    (void)context;
    (void)info;

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

    ESP_LOGI(TAG, "Recv msg_id=%08lx", (unsigned long)msg.msg_id);

    if (g_callback == NULL) {
        return;
    }

    thread_comms_message_t out;
    memset(&out, 0, sizeof(out));
    out.msg_id = msg.msg_id;

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

    esp_openthread_lock_acquire(portMAX_DELAY);

    /* Create OpenThread message */
    otMessage *ot_msg = otUdpNewMessage(instance, NULL);
    if (ot_msg == NULL) {
        ESP_LOGE(TAG, "Failed to allocate OT message");
        esp_openthread_lock_release();
        return ESP_ERR_NO_MEM;
    }

    otError err = otMessageAppend(ot_msg, buffer, stream.bytes_written);
    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "Failed to append message data: %d", err);
        otMessageFree(ot_msg);
        esp_openthread_lock_release();
        return ESP_FAIL;
    }

    /* Set destination - Realm-Local All Thread Nodes for SED compatibility */
    otMessageInfo info;
    memset(&info, 0, sizeof(info));
    const otIp6Address *multicast_addr = otThreadGetRealmLocalAllThreadNodesMulticastAddress(instance);
    memcpy(&info.mPeerAddr, multicast_addr, sizeof(otIp6Address));
    info.mPeerPort = THREAD_COMMS_PORT;

    err = otUdpSend(instance, &g_socket, ot_msg, &info);
    esp_openthread_lock_release();

    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "Failed to send UDP message: %d", err);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Sent msg_id=%08lx", (unsigned long)msg->msg_id);
    return ESP_OK;
}

/*── Public API ──*/

esp_err_t thread_comms_init(const char *device_id, thread_comms_source_t source)
{
    if (g_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    strncpy(g_device_id, device_id, sizeof(g_device_id) - 1);
    g_device_id[sizeof(g_device_id) - 1] = '\0';
    g_source = source;

    const char *type_str = (source == THREAD_COMMS_SOURCE_ROUTER) ? "router" : "end-device";
    ESP_LOGI(TAG, "Initializing as '%s' (%s)", device_id, type_str);

    /* OpenThread platform init */
    esp_openthread_platform_config_t ot_config = {
        .radio_config = { .radio_mode = RADIO_MODE_NATIVE },
        .host_config = { .host_connection_mode = HOST_CONNECTION_MODE_NONE },
        .port_config = { .storage_partition_name = "nvs", .netif_queue_size = 10, .task_queue_size = 10 },
    };
    ESP_ERROR_CHECK(esp_openthread_init(&ot_config));

    /* Create OpenThread netif */
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_OPENTHREAD();
    esp_netif_t *netif = esp_netif_new(&netif_cfg);
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_openthread_netif_glue_init(&ot_config)));

    otLoggingSetLevel(OT_LOG_LEVEL_NOTE);

    otInstance *instance = esp_openthread_get_instance();

    /* Configure dataset and enable Thread */
    esp_openthread_lock_acquire(portMAX_DELAY);
    configure_dataset(instance);
    otSetStateChangedCallback(instance, ot_state_changed, NULL);
    otIp6SetEnabled(instance, true);
    otThreadSetEnabled(instance, true);
    esp_openthread_lock_release();

    /* Start OpenThread mainloop task */
    xTaskCreate(ot_mainloop, "ot_mainloop", 4096, NULL, 5, NULL);

    /* Wait for network */
    wait_for_role(OT_DEVICE_ROLE_CHILD, "Waiting for Thread network...");
    ESP_LOGI(TAG, "Connected to Thread network");

    /* End-device: configure SED mode */
    if (source == THREAD_COMMS_SOURCE_END_DEVICE) {
        esp_openthread_lock_acquire(portMAX_DELAY);
        configure_sed_mode(instance);
        esp_openthread_lock_release();

        /* Wait for re-attachment after SED mode change */
        wait_for_role(OT_DEVICE_ROLE_CHILD, "Waiting to re-attach as SED...");
        ESP_LOGI(TAG, "Re-attached as SED");
    }

    /* Start UDP messaging */
    esp_err_t ret = start_udp();
    if (ret != ESP_OK) {
        return ret;
    }

    g_initialized = true;
    ESP_LOGI(TAG, "Thread comms ready");
    return ESP_OK;
}

void thread_comms_deinit(void)
{
    if (!g_initialized) {
        return;
    }

    otInstance *instance = esp_openthread_get_instance();
    if (instance != NULL) {
        esp_openthread_lock_acquire(portMAX_DELAY);
        otUdpClose(instance, &g_socket);
        esp_openthread_lock_release();
    }

    g_initialized = false;
    g_device_id[0] = '\0';
    g_callback = NULL;
    ESP_LOGI(TAG, "Thread comms stopped");
}

esp_err_t thread_comms_send_report(const thread_comms_report_t *report)
{
    if (!g_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    Message msg = Message_init_zero;
    msg.msg_id = generate_msg_id();
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
    if (!g_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    Message msg = Message_init_zero;
    msg.msg_id = generate_msg_id();
    msg.which_payload = Message_relay_cmd_tag;
    strncpy(msg.payload.relay_cmd.device_id, cmd->device_id, sizeof(msg.payload.relay_cmd.device_id) - 1);
    msg.payload.relay_cmd.relay_state = cmd->relay_state;

    return send_message(&msg);
}

void thread_comms_set_callback(thread_comms_callback_t callback)
{
    g_callback = callback;
}

void thread_comms_poll(void)
{
    if (!g_initialized || g_source != THREAD_COMMS_SOURCE_END_DEVICE) {
        return;
    }

    otInstance *instance = esp_openthread_get_instance();
    if (instance != NULL) {
        esp_openthread_lock_acquire(portMAX_DELAY);
        otLinkSendDataRequest(instance);
        esp_openthread_lock_release();
    }
}
