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
#include "freertos/timers.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "esp_wifi.h"
#include "esp_bridge.h"
#include "esp_mesh_lite.h"

#include "driver/uart.h"

#define TAG "mesh_root"

/* UART config */
#define UART_PORT     UART_NUM_1
#define UART_TX_PIN   17
#define UART_RX_PIN   16   // not used, but must be set
#define UART_BAUD     115200

/* UDP port used by leafs */
#define UDP_PORT      3333

/* ---------- storage & Wi-Fi init ---------- */
static esp_err_t esp_storage_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

static void wifi_init(void)
{
    wifi_config_t sta_cfg;
    memset(&sta_cfg, 0, sizeof(sta_cfg));
    esp_bridge_wifi_set_config(WIFI_IF_STA, &sta_cfg);

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid     = CONFIG_BRIDGE_SOFTAP_SSID,
            .password = CONFIG_BRIDGE_SOFTAP_PASSWORD,
            .channel  = CONFIG_MESH_CHANNEL,
        },
    };
    esp_bridge_wifi_set_config(WIFI_IF_AP, &ap_cfg);
}

static void app_wifi_set_softap_info(void)
{
    char ssid[33];
    char psw[64];
    uint8_t mac[6];
    size_t ssid_sz = sizeof(ssid);
    size_t psw_sz  = sizeof(psw);

    esp_wifi_get_mac(WIFI_IF_AP, mac);
    memset(ssid, 0, sizeof(ssid));
    memset(psw,  0, sizeof(psw));

    if (esp_mesh_lite_get_softap_ssid_from_nvs(ssid, &ssid_sz) != ESP_OK) {
#ifdef CONFIG_BRIDGE_SOFTAP_SSID_END_WITH_THE_MAC
        snprintf(ssid, sizeof(ssid), "%.25s_%02x%02x%02x",
                 CONFIG_BRIDGE_SOFTAP_SSID, mac[3], mac[4], mac[5]);
#else
        snprintf(ssid, sizeof(ssid), "%.32s", CONFIG_BRIDGE_SOFTAP_SSID);
#endif
    }
    if (esp_mesh_lite_get_softap_psw_from_nvs(psw, &psw_sz) != ESP_OK) {
        strlcpy(psw, CONFIG_BRIDGE_SOFTAP_PASSWORD, sizeof(psw));
    }
    esp_mesh_lite_set_softap_info(ssid, psw);
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
            buf[len] = 0; // null terminate
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
    esp_storage_init();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_bridge_create_all_netif();
    wifi_init();

    esp_mesh_lite_config_t cfg = ESP_MESH_LITE_DEFAULT_INIT();
    cfg.join_mesh_without_configured_wifi = false; // root
    ESP_ERROR_CHECK(esp_mesh_lite_init(&cfg));
    app_wifi_set_softap_info();

    ESP_LOGI(TAG, "Root node");
    esp_mesh_lite_set_allowed_level(1);
    ESP_ERROR_CHECK(esp_mesh_lite_start());

    root_uart_init();
    xTaskCreate(udp_listener_task, "udp_listener", 4096, NULL, 5, NULL);
}#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "esp_mesh_lite.h"
#include "uart_bridge.h"

static const char *TAG = "mesh_root";

/* RX callback: data from mesh → mirror to UART */
static void mesh_data_recv_cb(const uint8_t *data, uint16_t len, const mesh_addr_t *from)
{
    if (!data || !len) return;

    // Push raw bytes to UART
    uart_bridge_write(data, len);
    const char nl = '\n';
    uart_bridge_write((const uint8_t *)&nl, 1);

    if (from) {
        const uint8_t *m = (const uint8_t *)from;  // v1.0: MAC as 6-byte array
        ESP_LOGI(TAG, "RX %u bytes from %02X:%02X:%02X:%02X:%02X:%02X -> UART",
                 (unsigned)len, m[0], m[1], m[2], m[3], m[4], m[5]);
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    uart_bridge_init();

    esp_mesh_lite_config_t *cfg = esp_mesh_lite_get_ap_config();
    if (!cfg) {
        ESP_LOGE(TAG, "esp_mesh_lite_get_ap_config() returned NULL");
        abort();
    }
    ESP_ERROR_CHECK(esp_mesh_lite_init(cfg));

    // v1.0 API: register RX callback
    ESP_ERROR_CHECK(esp_mesh_lite_register_data_cb(mesh_data_recv_cb));

    ESP_ERROR_CHECK(esp_mesh_lite_start());

    ESP_LOGI(TAG, "Root up. Mirroring mesh payloads to UART%d TX=%d @%d",
             CONFIG_UART_BRIDGE_PORT, CONFIG_UART_BRIDGE_TX_PIN, CONFIG_UART_BRIDGE_BAUD);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
