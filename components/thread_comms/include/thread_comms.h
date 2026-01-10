#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*── Types ──*/

typedef enum {
    THREAD_COMMS_SOURCE_END_DEVICE,
    THREAD_COMMS_SOURCE_ROUTER,
} thread_comms_source_t;

typedef struct {
    char device_id[32];
    float temperature;
    float humidity;
    bool has_temperature;
    bool has_humidity;
    bool relay_state;
    bool has_relay_state;
} thread_comms_report_t;

typedef struct {
    char device_id[32];      /* Target device */
    bool relay_state;        /* Desired state */
} thread_comms_relay_cmd_t;

typedef enum {
    THREAD_COMMS_MSG_REPORT,
    THREAD_COMMS_MSG_RELAY_CMD,
} thread_comms_msg_type_t;

typedef struct {
    uint32_t msg_id;  /* Upper 16 bits: timestamp, lower 16 bits: random */
    thread_comms_msg_type_t type;
    union {
        thread_comms_report_t report;
        thread_comms_relay_cmd_t relay_cmd;
    };
} thread_comms_message_t;

typedef void (*thread_comms_callback_t)(const thread_comms_message_t *msg);

/**
 * @brief UART configuration for RCP connection
 */
typedef struct {
    int port;       /* UART port number (e.g., 1) */
    int tx_pin;     /* GPIO for TX */
    int rx_pin;     /* GPIO for RX */
} thread_comms_uart_config_t;

/**
 * @brief Thread comms initialization config
 */
typedef struct {
    const char *device_id;              /* Device identifier */
    thread_comms_source_t source;       /* End device or router */
    bool use_uart_rcp;                  /* true = UART to RCP, false = native radio */
    thread_comms_uart_config_t uart;    /* Only used if use_uart_rcp = true */
} thread_comms_config_t;

/*── Lifecycle ──*/

/**
 * @brief Initialize Thread networking and comms
 *
 * This function:
 * - Initializes OpenThread platform
 * - Configures network dataset (credentials)
 * - Joins/forms the Thread network (blocks until connected)
 * - For end-devices: configures SED mode
 * - Opens UDP socket for messaging
 *
 * Prerequisites: esp_netif_init(), esp_event_loop_create_default(),
 *                esp_vfs_eventfd_register() must be called first.
 *
 * @param config Initialization configuration
 * @return ESP_OK on success
 */
esp_err_t thread_comms_init(const thread_comms_config_t *config);

/**
 * @brief Deinitialize thread comms
 */
void thread_comms_deinit(void);

/*── Sending ──*/

/**
 * @brief Send a sensor report via UDP multicast
 * @param report Report data to send
 * @return ESP_OK on success
 */
esp_err_t thread_comms_send_report(const thread_comms_report_t *report);

/**
 * @brief Send a relay command via UDP multicast
 * @param cmd Relay command to send
 * @return ESP_OK on success
 */
esp_err_t thread_comms_send_relay_cmd(const thread_comms_relay_cmd_t *cmd);

/*── Receiving ──*/

/**
 * @brief Set callback for received messages
 * @param callback Function to call when a message is received (NULL to disable)
 */
void thread_comms_set_callback(thread_comms_callback_t callback);

/**
 * @brief Poll parent for buffered messages (SED only)
 *
 * For Sleepy End Devices, this triggers a data request to the parent
 * router to receive any buffered messages. Should be called periodically
 * in the main loop.
 *
 * No-op for routers.
 */
void thread_comms_poll(void);

#ifdef __cplusplus
}
#endif
