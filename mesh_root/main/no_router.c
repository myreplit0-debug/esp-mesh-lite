#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_mesh_lite.h"
#include "driver/uart.h"

static const char *TAG = "mesh_root";

/* UART config from sdkconfig.defaults */
#define UART_PORT      CONFIG_UART_BRIDGE_PORT
#define UART_BAUD      CONFIG_UART_BRIDGE_BAUD
#define UART_TX_PIN    CONFIG_UART_BRIDGE_TX_PIN
#define UART_RX_PIN    CONFIG_UART_BRIDGE_RX_PIN

#define UART_BUF_SIZE  1024

/* Mesh receive callback → forward to UART */
static void mesh_data_recv_cb(const uint8_t *data, uint16_t len, const mesh_addr_t *from)
{
    ESP_LOGI(TAG, "Mesh → UART: %.*s", len, (const char *)data);
    uart_write_bytes(UART_PORT, (const char *)data, len);
}

/* Task: read UART and send into mesh */
static void uart_reader_task(void *arg)
{
    uint8_t buf[UART_BUF_SIZE];
    while (1) {
        int len = uart_read_bytes(UART_PORT, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (len > 0) {
            ESP_LOGI(TAG, "UART → Mesh: %.*s", len, buf);
            esp_mesh_lite_send(NULL, buf, len);  // NULL = broadcast
        }
    }
}

/* Main app */
void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Init mesh lite */
    esp_mesh_lite_config_t cfg = {0};
    ESP_ERROR_CHECK(esp_mesh_lite_init(&cfg));
    ESP_ERROR_CHECK(esp_mesh_lite_start());

    /* Register RX callback */
    esp_mesh_lite_set_receive_cb(mesh_data_recv_cb);

    /* Setup UART */
    uart_config_t uart_cfg = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, UART_BUF_SIZE, UART_BUF_SIZE, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    xTaskCreate(uart_reader_task, "uart_reader", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Mesh root with UART bridge is up.");
}
