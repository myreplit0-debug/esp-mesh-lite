#include <string.h>
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef CONFIG_UART_BRIDGE_PORT
#define CONFIG_UART_BRIDGE_PORT  UART_NUM_1
#endif
#ifndef CONFIG_UART_BRIDGE_BAUD
#define CONFIG_UART_BRIDGE_BAUD  115200
#endif
#ifndef CONFIG_UART_BRIDGE_TX_PIN
#define CONFIG_UART_BRIDGE_TX_PIN 17
#endif
#ifndef CONFIG_UART_BRIDGE_RX_PIN
#define CONFIG_UART_BRIDGE_RX_PIN 16
#endif

static const char *TAG = "uart_bridge";

void uart_bridge_init(void)
{
    const uart_config_t uc = {
        .baud_rate = CONFIG_UART_BRIDGE_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(CONFIG_UART_BRIDGE_PORT, 2048, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(CONFIG_UART_BRIDGE_PORT, &uc));
    ESP_ERROR_CHECK(uart_set_pin(CONFIG_UART_BRIDGE_PORT,
                                 CONFIG_UART_BRIDGE_TX_PIN,
                                 CONFIG_UART_BRIDGE_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "UART ready @%d (TX=%d RX=%d)",
             CONFIG_UART_BRIDGE_BAUD, CONFIG_UART_BRIDGE_TX_PIN, CONFIG_UART_BRIDGE_RX_PIN);
}

void uart_bridge_write(const uint8_t *data, size_t len)
{
    if (!data || len == 0) return;        // <-- FIX: was `if(!data || len) return;`
    uart_write_bytes(CONFIG_UART_BRIDGE_PORT, (const char *)data, len);
}
