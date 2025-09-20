/*
 * Root node viewer:
 * - Mesh-Lite root
 * - UDP listener on :3333 (from leaves)
 * - SoftAP + HTTP page (http://192.168.5.1) to view messages live
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
#include "esp_netif.h"
#include "esp_mesh_lite.h"

#include "esp_http_server.h"

#define TAG "no_router_root"

#define UDP_PORT        3333
#define RBUF_LINES      100
#define LINE_MAX        256

/* -------- forward decls -------- */
static void sse_broadcast(const char *msg);

/* -------- ring buffer for recent messages -------- */
static char rbuf[RBUF_LINES][LINE_MAX];
static size_t rhead = 0, rcount = 0;
static portMUX_TYPE rlock = portMUX_INITIALIZER_UNLOCKED;

static inline void rbuf_push(const char *s) {
    taskENTER_CRITICAL(&rlock);
    // Copy message into ring buffer slot
    strlcpy(rbuf[rhead], s, LINE_MAX);
    rhead = (rhead + 1) % RBUF_LINES;
    if (rcount < RBUF_LINES) {
        rcount++;
    }
    taskEXIT_CRITICAL(&rlock);
}

/* -------- simple ring-buffer reader for HTML page -------- */
static void rbuf_dump(char *out, size_t max) {
    // Dump messages in chronological order (oldest first)
    size_t n = 0;
    taskENTER_CRITICAL(&rlock);
    size_t available = rcount;
    size_t idx = (rhead + RBUF_LINES - rcount) % RBUF_LINES;
    for (size_t i = 0; i < available; i++) {
        n += snprintf(out + n, (n < max ? max - n : 0),
                      "<div>%s</div>\n", rbuf[idx]);
        idx = (idx + 1) % RBUF_LINES;
    }
    taskEXIT_CRITICAL(&rlock);
}

/* -------- HTTP server: serve main page & SSE -------- */
static const char *INDEX_HTML = "<!DOCTYPE html>\n"
"<html>\n<head>\n<title>Mesh Root Viewer</title></head>\n<body>\n"
"<h2>Mesh-Lite Root – Recent Messages</h2>\n<div id='log' style='height: 300px; overflow-y: scroll; background: #f0f0f0; padding: 5px;'></div>\n"
"<script>\n"
"  const evtSource = new EventSource('/events');\n"
"  evtSource.onmessage = function(e) {\n"
"    const log = document.getElementById('log');\n"
"    log.innerHTML += `<div>${e.data}</div>`;\n"
"    log.scrollTop = log.scrollHeight;\n"
"  };\n"
"</script>\n"
"</body>\n</html>\n";

/* HTTP GET handler for the root page */
static esp_err_t handle_get_root(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* Server-Sent Events (SSE) handler for streaming messages */
typedef struct sse_client_s {
    httpd_handle_t hd;
    int fd;
    struct sse_client_s *next;
} sse_client_t;

static sse_client_t *g_clients = NULL;
static portMUX_TYPE sse_lock = portMUX_INITIALIZER_UNLOCKED;

/* Broadcast a message to all connected SSE clients */
static void sse_broadcast(const char *msg) {
    taskENTER_CRITICAL(&sse_lock);
    sse_client_t **pp = &g_clients;
    while (*pp) {
        sse_client_t *client = *pp;
        if (httpd_queue_work(client->hd, (void(*)(void*)){ 
                // This function runs in HTTP daemon's context to send the event
                httpd_resp_send_chunk(client->hd, msg, strlen(msg)) 
            }, NULL) != ESP_OK) {
            // If we fail to queue work, assume client disconnected
            *pp = client->next;
            close(client->fd);
            free(client);
            continue;
        }
        pp = &(*pp)->next;
    }
    taskEXIT_CRITICAL(&sse_lock);
}

/* HTTP handler for SSE endpoint "/events" */
static esp_err_t handle_get_events(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    // Construct a new client and add to list
    sse_client_t *client = calloc(1, sizeof(sse_client_t));
    if (!client) {
        return ESP_ERR_NO_MEM;
    }
    client->hd = req->handle;
    client->fd = httpd_req_to_sockfd(req);
    client->next = NULL;
    // Set as long-running (HTTPD_CLOSING_FLAG_NONE means keep handler active)
    httpd_req_set_pipe_mode(req, HTTPD_PIPE_MODE_BYTE);
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_send_chunk(req, "\n", 1);  // send a dummy chunk to open the stream

    // Add this client to our list
    taskENTER_CRITICAL(&sse_lock);
    sse_client_t **pp = &g_clients;
    while (*pp) pp = &(*pp)->next;
    *pp = client;
    taskEXIT_CRITICAL(&sse_lock);
    ESP_LOGI(TAG, "SSE client connected (fd=%d)", client->fd);
    return ESP_OK;
}

/* Start the HTTP server with URI handlers */
static esp_http_server_handle_t start_http_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;  // purge oldest if max open sockets exceeded
    esp_http_server_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root_page = {
            .uri      = "/",
            .method   = HTTP_GET,
            .handler  = handle_get_root,
            .user_ctx = NULL
        };
        httpd_uri_t events_endpoint = {
            .uri      = "/events",
            .method   = HTTP_GET,
            .handler  = handle_get_events,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &root_page);
        httpd_register_uri_handler(server, &events_endpoint);
    }
    return server;
}

/* -------- UDP listener task -------- */
static void udp_listen_task(void *pvParameters) {
    char rx_buf[LINE_MAX];
    char addr_str[128];
    int addr_family = AF_INET;
    int ip_protocol = IPPROTO_IP;
    struct sockaddr_in6 dest_addr;

    // Listen on UDP port 3333 on any IP (IPv4)
    struct sockaddr_in *dest_addr_ipv4 = (struct sockaddr_in *)&dest_addr;
    dest_addr_ipv4->sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr_ipv4->sin_family = AF_INET;
    dest_addr_ipv4->sin_port = htons(UDP_PORT);
    int sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create UDP socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "UDP listener socket created");

    int err = bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err < 0) {
        ESP_LOGE(TAG, "UDP bind failed: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Listening on UDP port %d", UDP_PORT);

    while (1) {
        struct sockaddr_storage source_addr;
        socklen_t socklen = sizeof(source_addr);
        int len = recvfrom(sock, rx_buf, sizeof(rx_buf) - 1, 0,
                           (struct sockaddr *)&source_addr, &socklen);
        if (len < 0) {
            ESP_LOGE(TAG, "UDP recvfrom failed: errno %d", errno);
            break;
        } else if (len == 0) {
            // Empty packet, ignore
            continue;
        }
        // Null-terminate the received data
        rx_buf[len] = '\0';

        // Get sender's address as string
        if (source_addr.ss_family == AF_INET) {
            inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str, sizeof(addr_str));
        }
        ESP_LOGI(TAG, "Received from %s: %s", addr_str, rx_buf);

        // Log to ring buffer and broadcast to HTTP SSE clients
        rbuf_push(rx_buf);
        char sse_msg[LINE_MAX + 10];
        snprintf(sse_msg, sizeof(sse_msg), "data: %s\n\n", rx_buf);
        sse_broadcast(sse_msg);
    }

    if (sock != -1) {
        ESP_LOGE(TAG, "Shutting down UDP socket.");
        close(sock);
    }
    vTaskDelete(NULL);
}

/* -------- storage & Wi-Fi config (no esp_bridge) -------- */
static esp_err_t esp_storage_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

static esp_err_t example_config(void) {
    // Configure device as both Station and SoftAP (Mesh root uses SoftAP; STA is idle)
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    // Station config (unused in no-router scenario, but needed to initialize interface)
    wifi_config_t sta_cfg = { 0 };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));

    // SoftAP config (uses configured SSID/PWD on fixed channel)
    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = CONFIG_BRIDGE_SOFTAP_SSID,
            .password = CONFIG_BRIDGE_SOFTAP_PASSWORD,
            .ssid_len = 0,
            .channel = CONFIG_MESH_CHANNEL,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
            .max_connection = 4,
        },
    };
    if (strlen(CONFIG_BRIDGE_SOFTAP_PASSWORD) == 0) {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi initialized. SoftAP SSID:%s Channel:%d", CONFIG_BRIDGE_SOFTAP_SSID, CONFIG_MESH_CHANNEL);
    return ESP_OK;
}

/* -------- Application entry point -------- */
void app_main(void) {
    // Initialize storage and Wi-Fi
    ESP_ERROR_CHECK(esp_storage_init());
    ESP_ERROR_CHECK(example_config());

    // Start Mesh-Lite (initializes internal mesh networking tasks)
    ESP_ERROR_CHECK(esp_mesh_lite_start());

    // Launch the UDP listener task
    xTaskCreate(udp_listen_task, "udp_listen", 4096, NULL, 5, NULL);

    // Start the HTTP server for web viewing
    start_http_server();

    // (Optional) Print basic info about this node and network
    uint8_t sta_mac[6];
    wifi_ap_record_t ap_info;
    (void)esp_wifi_get_mac(ESP_IF_WIFI_STA, sta_mac);
    (void)esp_wifi_sta_get_ap_info(&ap_info);
    ESP_LOGI(TAG, "System ready, Mesh-Lite node level=%d, softAP:%s connected (RSSI %d), free_heap=%" PRIu32,
             esp_mesh_lite_get_level(),
             CONFIG_BRIDGE_SOFTAP_SSID,
             (ap_info.rssi != 0 ? ap_info.rssi : -120),
             esp_get_free_heap_size());
}
