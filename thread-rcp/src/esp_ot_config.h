/*
 * Thread RCP Configuration
 *
 * Based on ESP-IDF ot_rcp example (CC0-1.0 licensed)
 */

#pragma once

#include "esp_openthread_types.h"
#include "driver/uart.h"

#define ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG()   \
    {                                           \
        .radio_mode = RADIO_MODE_NATIVE,        \
    }

#if CONFIG_OPENTHREAD_RCP_UART
/* UART mode - communicate with host via UART
 * ESP Thread Border Router board (per esp-thread-br README):
 *   H2 TX  -> S3 GPIO17 (RX)
 *   H2 RX  <- S3 GPIO18 (TX)
 * Use UART_PIN_NO_CHANGE to use H2's default UART TX/RX pins.
 */
#define ESP_OPENTHREAD_DEFAULT_HOST_CONFIG()                    \
    {                                                           \
        .host_connection_mode = HOST_CONNECTION_MODE_RCP_UART,  \
        .host_uart_config = {                                   \
            .port = 0,                                          \
            .uart_config =                                      \
                {                                               \
                    .baud_rate = 115200,                        \
                    .data_bits = UART_DATA_8_BITS,              \
                    .parity = UART_PARITY_DISABLE,              \
                    .stop_bits = UART_STOP_BITS_1,              \
                    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,      \
                    .rx_flow_ctrl_thresh = 0,                   \
                    .source_clk = UART_SCLK_XTAL,               \
                },                                              \
            .rx_pin = UART_PIN_NO_CHANGE,                       \
            .tx_pin = UART_PIN_NO_CHANGE,                       \
        },                                                      \
    }

#elif CONFIG_OPENTHREAD_RCP_SPI
/* SPI mode - communicate with host via SPI */
#define ESP_OPENTHREAD_DEFAULT_HOST_CONFIG()                    \
    {                                                           \
        .host_connection_mode = HOST_CONNECTION_MODE_RCP_SPI,   \
        .spi_slave_config = {                                   \
            .host_device = SPI2_HOST,                           \
            .bus_config = {                                     \
                .mosi_io_num = 3,                               \
                .miso_io_num = 1,                               \
                .sclk_io_num = 0,                               \
                .quadhd_io_num = -1,                            \
                .quadwp_io_num = -1,                            \
                .isr_cpu_id = ESP_INTR_CPU_AFFINITY_AUTO,       \
            },                                                  \
            .slave_config = {                                   \
                .mode = 0,                                      \
                .spics_io_num = 2,                              \
                .queue_size = 3,                                \
                .flags = 0,                                     \
            },                                                  \
            .intr_pin = 9,                                      \
        },                                                      \
    }

#else
/* USB Serial/JTAG mode (default) */
#define ESP_OPENTHREAD_DEFAULT_HOST_CONFIG()                        \
    {                                                               \
        .host_connection_mode = HOST_CONNECTION_MODE_RCP_USB,       \
        .host_usb_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT(), \
    }
#endif

#define ESP_OPENTHREAD_DEFAULT_PORT_CONFIG()    \
    {                                           \
        .storage_partition_name = "nvs",        \
        .netif_queue_size = 10,                 \
        .task_queue_size = 10,                  \
    }
