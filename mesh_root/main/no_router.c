/*
 * Root node: UDP :3333 from leaves -> TCP stream to one client on SoftAP
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_mesh_lite.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#define TAG "mesh_root"

#define UDP_PORT    3333     // leaves send here
#define TCP_PORT    5000     // client connects here on SoftAP IP (usually 192.168.4.1)

static esp_err_t esp_storage_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    return ret;
}

/* ---- Simple single-client TCP server on SoftAP IP ---- */
static volatile int s_tcp_client = -1;

static void tcp_server_task(void *arg)
{
    int srv = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (srv < 0) { ESP_LOGE(TAG, "TCP socket create failed"); vTaskDelete(NULL); }

    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(TCP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "TCP bind failed"); close(srv); vTaskDelete(NULL);
    }
    listen(srv, 1);
    ESP_LOGI(TAG, "TCP server listening on %d (connect to root SoftAP IP, usually 192.168.4.1)", TCP_PORT);

    for (;;) {
        struct sockaddr_in cli; socklen_t slen = sizeof(cli);
        int c = accept(srv, (struct sockaddr *)&cli, &slen);
        if (c < 0) continue;

        if (s_tcp_client >= 0) { close(s_tcp_client); }
        s_tcp_client = c;

        char ip[16]; inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip));
        ESP_LOGI(TAG, "TCP client connected from %s:%d", ip, ntohs(cli.sin_port));
    }
}

/* ---- UDP listener from leaves -> forward to TCP client ---- */
static void udp_in_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { ESP_LOGE(TAG, "UDP socket create failed"); vTaskDelete(NULL); }

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(UDP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "UDP bind failed"); close(sock); vTaskDelete(NULL);
    }

    ESP_LOGI(TAG, "Root listening for leaf UDP on %d", UDP_PORT);

    for (;;) {
        char buf[256];
        int len = recvfrom(sock, buf, sizeof(buf)-1, 0, NULL, NULL);
        if (len <= 0) continue;
        buf[len] = 0;

        ESP_LOGI(TAG, "UDP <-leaf: %s", buf);

        int c = s_tcp_client;
        if (c >= 0) {
            // Append CRLF so the client can read line-based
            send(c, buf, len, 0);
            send(c, "\r\n", 2, 0);
        }
    }
}

/* --------------------------- app_main --------------------------- */
void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_storage_init();

    esp_netif_init();
    esp_event_loop_create_default();

    // Mesh-Lite config in this repo returns void from init/start
    esp_mesh_lite_config_t cfg = ESP_MESH_LITE_DEFAULT_INIT();
    cfg.join_mesh_ignore_router_status = true;
    cfg.join_mesh_without_configured_wifi = true;   // root without upstream router

    esp_mesh_lite_init(&cfg);
    esp_mesh_lite_set_allowed_level(1);             // stay root
    esp_mesh_lite_start();

    xTaskCreate(tcp_server_task, "tcp_server", 4096, NULL, 5, NULL);
    xTaskCreate(udp_in_task,   "udp_in",     4096, NULL, 5, NULL);
}
