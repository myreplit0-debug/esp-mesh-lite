/*
 * Root node: Mesh-Lite UDP :3333 (from leaves) -> re-broadcast to SoftAP 192.168.4.255:3333
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <inttypes.h>
#include <sys/socket.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "esp_mesh_lite.h"
#include "esp_bridge.h"

static const char *TAG = "root_udp";

/* Leaves are sending to 3333 already */
#define UDP_PORT          3333
/* Default ESP-IDF SoftAP network is 192.168.4.0/24 -> broadcast 192.168.4.255 */
#define SOFTAP_BCAST_IP   "192.168.4.255"

/* ---------- NVS (storage) ---------- */
static esp_err_t storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

/* ---------- Wi-Fi ---------- */
static void wifi_init(void)
{
    /* We let Mesh-Lite/bridge own netifs; just hand it blank cfgs so it uses Kconfig defaults */
    wifi_config_t sta = {0};
    wifi_config_t ap  = {
        .ap = {
            .ssid     = CONFIG_BRIDGE_SOFTAP_SSID,
            .password = CONFIG_BRIDGE_SOFTAP_PASSWORD,
            .channel  = CONFIG_MESH_CHANNEL,
        }
    };
    esp_bridge_wifi_set_config(WIFI_IF_STA, &sta);
    esp_bridge_wifi_set_config(WIFI_IF_AP,  &ap);
}

static void set_softap_ssid_psw_from_nvs_or_default(void)
{
    char ssid[33] = {0};
    char psw[64]  = {0};
    size_t ssid_sz = sizeof(ssid);
    size_t psw_sz  = sizeof(psw);

    if (esp_mesh_lite_get_softap_ssid_from_nvs(ssid, &ssid_sz) != ESP_OK) {
#ifdef CONFIG_BRIDGE_SOFTAP_SSID_END_WITH_THE_MAC
        uint8_t mac[6];
        esp_wifi_get_mac(WIFI_IF_AP, mac);
        snprintf(ssid, sizeof(ssid), "%.25s_%02x%02x%02x",
                 CONFIG_BRIDGE_SOFTAP_SSID, mac[3], mac[4], mac[5]);
#else
        strlcpy(ssid, CONFIG_BRIDGE_SOFTAP_SSID, sizeof(ssid));
#endif
    }
    if (esp_mesh_lite_get_softap_psw_from_nvs(psw, &psw_sz) != ESP_OK) {
        strlcpy(psw, CONFIG_BRIDGE_SOFTAP_PASSWORD, sizeof(psw));
    }
    esp_mesh_lite_set_softap_info(ssid, psw);
}

/* ---------- Info print (optional) ---------- */
static void print_info_cb(TimerHandle_t xTimer)
{
    uint8_t sta_mac[6] = {0};
    wifi_ap_record_t ap_info = {0};
    esp_wifi_get_mac(ESP_IF_WIFI_STA, sta_mac);
    esp_wifi_sta_get_ap_info(&ap_info);

    ESP_LOGI(TAG, "layer:%d self:" MACSTR " parent:" MACSTR " rssi:%d heap:%" PRIu32,
             esp_mesh_lite_get_level(),
             MAC2STR(sta_mac), MAC2STR(ap_info.bssid),
             (ap_info.rssi ? ap_info.rssi : -120),
             esp_get_free_heap_size());
}

/* ---------- UDP bridge: mesh -> SoftAP broadcast ---------- */
static void udp_bridge_task(void *arg)
{
    /* Socket to RECEIVE from leaves (mesh side) */
    int rx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ESP_ERROR_CHECK((rx < 0) ? ESP_FAIL : ESP_OK);

    int reuse = 1;
    setsockopt(rx, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in bind_addr = {0};
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_port        = htons(UDP_PORT);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    ESP_ERROR_CHECK(bind(rx, (struct sockaddr *)&bind_addr, sizeof(bind_addr)));

    /* Socket to SEND to phone (SoftAP broadcast) */
    int tx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ESP_ERROR_CHECK((tx < 0) ? ESP_FAIL : ESP_OK);
    int yes = 1;
    setsockopt(tx, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

    struct sockaddr_in ap_bcast = {0};
    ap_bcast.sin_family      = AF_INET;
    ap_bcast.sin_port        = htons(UDP_PORT);
    ap_bcast.sin_addr.s_addr = inet_addr(SOFTAP_BCAST_IP);   // 192.168.4.255

    ESP_LOGI(TAG, "UDP bridge up: listen :%d, broadcast -> %s:%d",
             UDP_PORT, SOFTAP_BCAST_IP, UDP_PORT);

    uint8_t buf[1024];

    while (1) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int n = recvfrom(rx, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
        if (n <= 0) continue;

        /* Log a preview (safe for text lines) */
        int m = n > 80 ? 80 : n;
        char preview[81];
        memcpy(preview, buf, m);
        preview[m] = 0;
        for (int i = 0; i < m; ++i) {      // make log-friendly
            if (preview[i] < 32 || preview[i] > 126) preview[i] = '.';
        }
        ESP_LOGI(TAG, "RX %dB from %s:%d : %s%s",
                 n, inet_ntoa(from.sin_addr), ntohs(from.sin_port),
                 preview, (n > m ? "..." : ""));

        /* Re-broadcast so phone clients (connected to AP) can see it */
        sendto(tx, buf, n, 0, (struct sockaddr *)&ap_bcast, sizeof(ap_bcast));
    }
}

/* --------------------------- app_main --------------------------- */
void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);

    ESP_ERROR_CHECK(storage_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Create default netifs (STA+AP) managed by bridge */
    esp_bridge_create_all_netif();

    wifi_init();

    /* Mesh-Lite root config */
    esp_mesh_lite_config_t cfg = ESP_MESH_LITE_DEFAULT_INIT();
    cfg.join_mesh_ignore_router_status        = true;   // no upstream router required
    cfg.join_mesh_without_configured_wifi     = false;  // we are ROOT only
    ESP_ERROR_CHECK(esp_mesh_lite_init(&cfg));

    set_softap_ssid_psw_from_nvs_or_default();

    /* Force this device to be ROOT */
    esp_mesh_lite_set_allowed_level(1);   // only level 1 allowed -> we become root
    ESP_ERROR_CHECK(esp_mesh_lite_start());

    /* Start UDP bridge */
    xTaskCreate(udp_bridge_task, "udp_bridge", 4096, NULL, 5, NULL);

    /* Optional periodic info */
    TimerHandle_t t = xTimerCreate("info", pdMS_TO_TICKS(10000), pdTRUE, NULL, print_info_cb);
    xTimerStart(t, 0);

    ESP_LOGI(TAG, "Root ready. Connect your phone to the SoftAP and listen UDP :%d", UDP_PORT);
}
