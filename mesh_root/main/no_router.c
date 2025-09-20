#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_mesh_lite.h"
#include "driver/uart.h"

static const char *TAG = "mesh_root";

#define UART_PORT   CONFIG_UART_BRIDGE_PORT
#define UART_TX_PIN CONFIG_UART_BRIDGE_TX_PIN
#define UART_RX_PIN CONFIG_UART_BRIDGE_RX_PIN
#define UART_BAUD   CONFIG_UART_BRIDGE_BAUD

/* Mesh event callback */
static void mesh_event_cb(mesh_lite_event_t event, void *arg)
{
    if (event.id == MESH_LITE_EVENT_RECV) {
        mesh_lite_event_recv_t *recv = (mesh_lite_event_recv_t *)arg;
        ESP_LOGI(TAG, "RX from "MACSTR" len=%d",
                 MAC2STR(recv->src_addr), recv->size);

        // forward payload to UART
        uart_write_bytes(UART_PORT, (const char *)recv->data, recv->size);
        uart_write_bytes(UART_PORT, "\n", 1);
    }
}

void app_main(void)
{
    // Init NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Configure as root
    esp_mesh_lite_config_t cfg = ESP_MESH_LITE_DEFAULT_CONFIG();
    cfg.only_root = true;
    cfg.event_cb  = mesh_event_cb;

    ESP_ERROR_CHECK(esp_mesh_lite_init(&cfg));
    ESP_ERROR_CHECK(esp_mesh_lite_start());

    // Setup UART
    uart_config_t uart_cfg = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, 2048, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "Mesh root started, UART bridge ready (TX=%d RX=%d @%d)",
             UART_TX_PIN, UART_RX_PIN, UART_BAUD);
}
