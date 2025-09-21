/*
 * Root node viewer:
 * - Mesh-Lite root
 * - UDP listener on :3333 (from leaves)
 * - SoftAP + HTTP page (http://192.168.5.1) to view messages live
 * - Mirrors each received line to UART2 (TX=17) for an external ESP32/UI
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_mesh_lite.h"
#include "esp_bridge.h"

#include "esp_http_server.h"

/* === UART mirror (root -> external UI) === */
#include "driver/uart.h"
#include "driver/gpio.h"
#define UART_PORT   UART_NUM_2     // use UART2 for mirror
#define UART_TX_PIN 17             // Root TX -> UI RX
#define UART_RX_PIN 16             // not used here but must be set
#define UART_BAUD   9600           // match your UI device

#define TAG "no_router_root"

#define UDP_PORT        3333
#define RBUF_LINES      100
#define LINE_MAX        256

/* -------- ring buffer for recent messages -------- */
static char rbuf[RBUF_LINES][LINE_MAX];
static size_t rhead = 0, rcount = 0;
static portMUX_TYPE rlock = portMUX_INITIALIZER_UNLOCKED;

static inline void rbuf_push(const char *s) {
    taskENTER_CRITICAL(&rlock);
    strlcpy(rbuf[rhead], s, LINE_MAX);
    rhead = (rhead + 1) % RBUF_LINES;
    if (rcount < RBUF_LINES) rcount++;
    taskEXIT_CRITICAL(&rlock);
}

/* broadcast queue to HTTP SSE clients */
typedef struct sse_client_s {
    httpd_handle_t hd;
    int fd;
    struct sse_client_s *next;
} sse_client_t;

static sse_client_t *g_clients = NULL;
static portMUX_TYPE sse_lock = portMUX_INITIALIZER_UNLOCKED;

static void sse_broadcast(const char *msg) {
    taskENTER_CRITICAL(&sse_lock);
    sse_client_t **pp = &g_clients;
    while (*pp) {
        sse_client_t *c = *pp;
        char buf[LINE_MAX + 16];
        int n = snprintf(buf, sizeof(buf), "data: %s\n\n", msg);
        if (httpd_socket_send(c->hd, c->fd, buf, n, 0) < 0) {
            // drop dead client
            *pp = c->next;
            free(c);
            continue;
        }
        pp = &(*pp)->next;
    }
    taskEXIT_CRITICAL(&sse_lock);
}

/* -------- mesh info print (from example) -------- */
static void print_system_info_timercb(TimerHandle_t xTimer) {
    uint8_t primary = 0;
    wifi_ap_record_t ap_info = (wifi_ap_record_t){0};
    wifi_second_chan_t second = 0;
    uint8_t sta_mac[6] = {0};

    if (esp_mesh_lite_get_level() > 1) {
        (void)esp_wifi_sta_get_ap_info(&ap_info);
    }
    (void)esp_wifi_get_mac(ESP_IF_WIFI_STA, sta_mac);
    (void)esp_wifi_get_channel(&primary, &second);

    ESP_LOGI(TAG, "System info: channel:%d layer:%d self:" MACSTR 
             " parent:" MACSTR " rssi:%d heap:%" PRIu32,
             primary, esp_mesh_lite_get_level(),
             MAC2STR(sta_mac), MAC2STR(ap_info.bssid),
             (ap_info.rssi != 0 ? ap_info.rssi : -120), esp_get_free_heap_size());
}

/* -------- storage & Wi-Fi config -------- */
static esp_err_t esp_storage_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

static void wifi_init(void) {
    // Station configuration (leave SSID/PW empty to use any saved credentials or none)
    wifi_config_t sta_cfg = (wifi_config_t){0};
    esp_bridge_wifi_set_config(WIFI_IF_STA, &sta_cfg);

    // SoftAP configuration for the root (credentials from sdkconfig.defaults)
    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = CONFIG_BRIDGE_SOFTAP_SSID,
            .password = CONFIG_BRIDGE_SOFTAP_PASSWORD,
            .ssid_len = 0,
            .channel  = CONFIG_MESH_CHANNEL,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
            .max_connection = 4,
        },
    };
    if (strlen(CONFIG_BRIDGE_SOFTAP_PASSWORD) == 0) {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }
    esp_bridge_wifi_set_config(WIFI_IF_AP, &ap_cfg);
}

static void app_wifi_set_softap_info(void) {
    char ssid[33] = {0};
    char psw[64]  = {0};
    size_t ssid_sz = sizeof(ssid), psw_sz = sizeof(psw);

    if (esp_mesh_lite_get_softap_ssid_from_nvs(ssid, &ssid_sz) != ESP_OK) {
        strlcpy(ssid, CONFIG_BRIDGE_SOFTAP_SSID, sizeof(ssid));
    }
    if (esp_mesh_lite_get_softap_psw_from_nvs(psw, &psw_sz) != ESP_OK) {
        strlcpy(psw, CONFIG_BRIDGE_SOFTAP_PASSWORD, sizeof(psw));
    }
    esp_mesh_lite_set_softap_info(ssid, psw);
}

/* -------- UART init (root mirror) -------- */
static void root_uart_init(void) {
    const uart_config_t cfg = {
        .baud_rate  = UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, 2048, 0, 0, NULL, 0));
