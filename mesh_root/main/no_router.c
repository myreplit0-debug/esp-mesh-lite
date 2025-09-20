/*
 * Root node: UDP :3333 listener -> UART TX (GPIO17)
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_system.h"

#include "esp_wifi.h"
#include "esp_mesh_lite.h"

#include "driver/uart.h"

#define TAG "mesh_root"

/* UART config */
#define UART_PORT     UART_NUM_1
#define UART_TX_PIN   17
#define UART_RX_PIN   16   // unused
#define UART_BAUD     115200

/* UDP port used by leafs */
#define UDP_PORT      3333

/* ---------- storage & Wi-Fi init ---------- */
static void storage_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
}

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* ---------- UART init ---------- */
static void root_uart_init(void)
{
    const uart_config_t cfg = {
        .baud_rate  = UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, 2048, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "UART ready TX=%d RX=%d baud=%d", UART_TX_PIN, UART_RX_PIN, UART_BAUD);
}

/* ---------- UDP listener -> UART ---------- */
static void udp_listener_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "UDP socket create failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(UDP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "UDP bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Root listening UDP port %d", UDP_PORT);

    char buf[256];
    for (;;) {
        int len = recv(sock, buf, sizeof(buf) - 1, 0);
        if (len > 0) {
            buf[len] = 0;
            ESP_LOGI(TAG, "UDP RX: %s", buf);
            uart_write_bytes(UART_PORT, buf, len);
            uart_write_bytes(UART_PORT, "\n", 1);
        }
    }
}

/* --------------------------- app_main --------------------------- */
void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);

    storage_init();
    wifi_init();

    esp_mesh_lite_config_t cfg = ESP_MESH_LITE_DEFAULT_INIT();
    cfg.join_mesh_without_configured_wifi = false; // force root
    ESP_ERROR_CHECK(esp_mesh_lite_init(&cfg));

    ESP_LOGI(TAG, "Root node");
    esp_mesh_lite_set_allowed_level(1);
    ESP_ERROR_CHECK(esp_mesh_lite_start());

    root_uart_init();
    xTaskCreate(udp_listener_task, "udp_listener", 4096, NULL, 5, NULL);
}
