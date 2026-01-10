# ESP Thread Border Router Board Configuration

## Hardware Wiring

The ESP Thread Border Router board connects ESP32-S3 and ESP32-H2 via UART:

```
ESP32-S3 (Host)              ESP32-H2 (RCP)
┌─────────────────┐          ┌─────────────────┐
│  GPIO17 (RX)    │◄─────────│  TXD0 (default) │
│  GPIO18 (TX)    │─────────►│  RXD0 (default) │
└─────────────────┘          └─────────────────┘
```

## Critical Configuration

### UART Clock Source

**MUST use `UART_SCLK_XTAL`** on both S3 and H2 for accurate baud rates.

Using `UART_SCLK_DEFAULT` causes baud rate mismatch and garbled communication.

### H2 RCP UART Settings (esp_ot_config.h)

```c
.host_uart_config = {
    .port = 0,
    .uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_XTAL,  // Critical!
    },
    .rx_pin = UART_PIN_NO_CHANGE,  // Use default RXD0
    .tx_pin = UART_PIN_NO_CHANGE,  // Use default TXD0
},
```

### S3 Host UART Settings (thread_comms.c)

```c
uart_config_t uart_config = {
    .baud_rate = 115200,
    .data_bits = UART_DATA_8_BITS,
    .parity = UART_PARITY_DISABLE,
    .stop_bits = UART_STOP_BITS_1,
    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    .source_clk = UART_SCLK_XTAL,  // Critical!
};

// S3 pins
uart_set_pin(UART_NUM_1, 18, 17, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
//                       TX  RX
```

## Pin Summary

| S3 Pin | Direction | H2 Pin |
|--------|-----------|--------|
| GPIO17 | RX ←      | TXD0   |
| GPIO18 | TX →      | RXD0   |

## Tested Baud Rates

- 115200: Working
- 460800: Not yet tested with XTAL clock
