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
#include <unistd.h>

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
#define UART_PORT   UART_NUM_2
#define UART_TX_PIN 17
#define UART_RX_PIN 16
#define UART_BAUD   9600

#define TAG "no_router_root"

#define UDP_PORT        3333
#define RBUF_LINES      100
#define MY_LINE_MAX     256   // avoid conflict with system LINE_MAX

/* -------- ring buffer for recent messages -------- */
static char rbuf[RBUF_LINES][MY_LINE_MAX];
static size_t rhead = 0, rcount = 0;
static portMUX_TYPE rlock = portMUX_INITIALIZER_UNLOCKED;

static inline void rbuf_push(const char *s) {
    taskENTER_CRITICAL(&rlock);
    strlcpy(rbuf[rhead], s, MY_LINE_MAX);
    rhead = (rhead + 1) % RBUF_LINES;
    if (rcount < RBUF_LINES) rcount++;
    taskEXIT_CRITICAL(&rlock);
}

/* -------- SSE client list -------- */
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
        char buf[MY_LINE_MAX + 16];
        int n = snprintf(buf, sizeof(buf), "data: %s\n\n", msg);
        if (httpd_socket_send(c->hd, c->fd, buf, n, 0) < 0) {
            *pp = c->next;
            free(c);
            continue;
        }
        pp = &(*pp)->next;
    }
    taskEXIT_CRITICAL(&sse_lock);
}

/* -------- periodic info -------- */
static void print_system_info_timercb(TimerHandle_t xTimer) {
    (void)xTimer;
    uint8_t primary = 0;
    wifi_second_chan_t second = 0;
    wifi_ap_record_t ap_info = (wifi_ap_record_t){0};

    if (esp_mesh_lite_get_level() > 1) {
        (void)esp_wifi_sta_get_ap_info(&ap_info);
    }
    (void)esp_wifi_get_channel(&primary, &second);

    ESP_LOGI(TAG, "System info: channel:%u layer:%u rssi:%d heap:%" PRIu32,
             (unsigned)primary,
             (unsigned)esp_mesh_lite_get_level(),
             (ap_info.rssi != 0 ? ap_info.rssi : -120),
             (uint32_t)esp_get_free_heap_size());
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
    wifi_config_t sta_cfg = (wifi_config_t){0};
    esp_bridge_wifi_set_config(WIFI_IF_STA, &sta_cfg);

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
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << UART_RX_PIN,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = 1,
        .pull_down_en = 0,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    ESP_LOGI(TAG, "UART ready @%d (TX=%d RX=%d)", UART_BAUD, UART_TX_PIN, UART_RX_PIN);
}

/* -------- UDP listener task -------- */
static void udp_listener_task(void *arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(UDP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind() failed on :%d", UDP_PORT);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "UDP listener ready on :%d", UDP_PORT);

    for (;;) {
        char buf[MY_LINE_MAX];
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &flen);
        if (n > 0) {
            buf[n] = '\0';
            ESP_LOGI(TAG, "RX %dB from %s: %s", n, inet_ntoa(from.sin_addr), buf);
            rbuf_push(buf);
            sse_broadcast(buf);

            uart_write_bytes(UART_PORT, buf, n);
            uart_write_bytes(UART_PORT, "\n", 1);
        }
    }
}

/* -------- HTTP server -------- */
static const char *INDEX_HTML =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>Mesh Viewer</title>"
"<style>body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial;margin:0;padding:1rem;"
"background:#111;color:#eee}#log{white-space:pre-wrap;font-family:ui-monospace,Consolas,monospace}"
".msg{padding:.25rem .5rem;border-bottom:1px solid #333}</style></head><body>"
"<h2>Mesh Viewer (UDP :3333)</h2><div id=log></div>"
"<script>"
"const log=document.getElementById('log');"
"function add(s){const d=document.createElement('div');d.className='msg';d.textContent=s;"
"log.prepend(d);const m=log.children;while(m.length>200)log.removeChild(log.lastChild);}"
"fetch('/history').then(r=>r.json()).then(a=>a.forEach(add));"
"const es=new EventSource('/events'); es.onmessage=e=>add(e.data);"
"</script></body></html>";

static esp_err_t root_get_handler(httpd_req_t *req) {
    (void) httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t history_get_handler(httpd_req_t *req) {
    (void) httpd_resp_set_type(req, "application/json");
    (void) httpd_resp_sendstr_chunk(req, "[");
    taskENTER_CRITICAL(&rlock);
    size_t n = rcount;
    for (size_t i = 0; i < n; ++i) {
        size_t idx = (rhead + RBUF_LINES - 1 - i) % RBUF_LINES;
        char esc[MY_LINE_MAX * 2];
        int p = 0;
        for (const char *s = rbuf[idx]; *s && p < (int)sizeof(esc) - 2; ++s) {
            if (*s == '\\' || *s == '\"') { esc[p++] = '\\'; esc[p++] = *s; }
            else if (*s == '\r') { continue; }
            else if (*s == '\n') { esc[p++] = ' '; }
            else { esc[p++] = *s; }
        }
        esc[p] = '\0';
        char chunk[MY_LINE_MAX * 2 + 8];
        snprintf(chunk, sizeof(chunk), "%s\"%s\"", (i == 0 ? "" : ","), esc);
        (void) httpd_resp_sendstr_chunk(req, chunk);
    }
    taskEXIT_CRITICAL(&rlock);
    (void) httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t events_get_handler(httpd_req_t *req) {
    (void) httpd_resp_set_type(req, "text/event-stream");
    (void) httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    (void) httpd_resp_set_hdr(req, "Connection", "keep-alive");
    (void) httpd_resp_sendstr_chunk(req, ":ok\n\n");

    sse_client_t *c = calloc(1, sizeof(*c));
    c->hd = req->handle;
    c->fd = httpd_req_to_sockfd(req);
    taskENTER_CRITICAL(&sse_lock);
    c->next = g_clients;
    g_clients = c;
    taskEXIT_CRITICAL(&sse_lock);

    while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    return ESP_OK;
}

static httpd_handle_t start_httpd(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.server_port = 80;

    httpd_handle_t hd = NULL;
    if (httpd_start(&hd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return NULL;
    }
    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler };
    httpd_uri_t history = { .uri = "/history", .method = HTTP_GET, .handler = history_get_handler };
    httpd_uri_t events = { .uri = "/events", .method = HTTP_GET, .handler = events_get_handler };
    (void) httpd_register_uri_handler(hd, &root);
    (void) httpd_register_uri_handler(hd, &history);
    (void) httpd_register_uri_handler(hd, &events);
    ESP_LOGI(TAG, "HTTP server ready at http://192.168.5.1/");
    return hd;
}

/* --------------------------- app_main --------------------------- */
void app_main(void) {
    ESP_ERROR_CHECK(esp_storage_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_bridge_create_all_netif();
    wifi_init();

    esp_mesh_lite_config_t cfg = ESP_MESH_LITE_DEFAULT_INIT();
    cfg.join_mesh_ignore_router_status = true;
    cfg.join_mesh_without_configured_wifi = true;   // allow boot with no STA creds
    ESP_LOGI(TAG, "Mesh-Lite init…");
    esp_mesh_lite_init(&cfg);   // void in this version

    app_wifi_set_softap_info();

    ESP_LOGI(TAG, "Root node; starting Mesh-Lite…");
    esp_mesh_lite_set_allowed_level(1);
    esp_mesh_lite_start();      // void in this version
    ESP_LOGI(TAG, "Mesh-Lite started.");

    // --- AP-only: stop STA reconnect spam ---
    ESP_ERROR_CHECK(esp_wifi_set_auto_connect(false));
    ESP_ERROR_CHECK(esp_wifi_disconnect());
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    root_uart_init();

    (void) start_httpd();
    xTaskCreate(udp_listener_task, "udp_listener", 4096, NULL, 5, NULL);

    TimerHandle_t t = xTimerCreate("print_system_info", pdMS_TO_TICKS(10000),
                                   pdTRUE, NULL, print_system_info_timercb);
    if (t) { xTimerStart(t, 0); }
}
