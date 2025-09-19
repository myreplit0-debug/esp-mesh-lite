// Root: receive UDP :3333 from leaves -> forward to UART (TX=17)
#include <string.h>
#include <sys/socket.h>
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "esp_mesh_lite.h"
#include "uart_bridge.h"

#define UDP_PORT    3333
#define FORCE_ROOT  1

static const char *TAG = "mesh_root";

/* ---------------- NVS ---------------- */
static esp_err_t esp_storage_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

/* ---------------- Basic Wi-Fi bring-up (no esp_bridge needed) ---------------- */
static void wifi_init_root(void)
{
    // Create default netifs (STA + AP)
    esp_netif_init();
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wicfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wicfg));

    // Start in AP+STA; Mesh-Lite will manage details on top
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    // SoftAP parameters for lower nodes to associate with this root if needed
    wifi_config_t ap_cfg = { 0 };
    strlcpy((char*)ap_cfg.ap.ssid,      CONFIG_BRIDGE_SOFTAP_SSID, sizeof(ap_cfg.ap.ssid));
    strlcpy((char*)ap_cfg.ap.password,  CONFIG_BRIDGE_SOFTAP_PASSWORD, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.ssid_len   = strlen((char*)ap_cfg.ap.ssid);
    ap_cfg.ap.channel    = CONFIG_MESH_CHANNEL;
    ap_cfg.ap.max_connection = 10;
    ap_cfg.ap.authmode   = (strlen((char*)ap_cfg.ap.password) >= 8) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    if (ap_cfg.ap.authmode == WIFI_AUTH_OPEN) {
        ap_cfg.ap.password[0] = '\0';
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    ESP_ERROR_CHECK(esp_wifi_start());
}

/* ---------------- UDP listener ---------------- */
static void udp_rx_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed");
        vTaskDelete(NULL);
        return;
    }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in local = {0};
    local.sin_family      = AF_INET;
    local.sin_port        = htons(UDP_PORT);
    local.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        ESP_LOGE(TAG, "bind() failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Listening UDP :%d and forwarding to UART…", UDP_PORT);

    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int r = recvfrom(sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&from, &fromlen);
        if (r > 0) {
            ESP_LOGI(TAG, "RX %dB from %s", r, inet_ntoa(from.sin_addr));
            uart_bridge_write(buf, (size_t)r);
            // If your DevKit expects newline-delimited messages, uncomment:
            // const char nl = '\n'; uart_bridge_write((const uint8_t*)&nl, 1);
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

/* ---------------- periodic log (optional) ---------------- */
static void print_system_info_timercb(TimerHandle_t x)
{
    (void)x;
    uint8_t ch=0; wifi_second_chan_t sc=0; esp_wifi_get_channel(&ch, &sc);
    uint8_t mac[6]={0}; esp_wifi_get_mac(ESP_IF_WIFI_STA, mac);
    ESP_LOGI(TAG, "Ch%u Level %d Self " MACSTR " Heap %u",
             ch, esp_mesh_lite_get_level(), MAC2STR(mac), (unsigned)esp_get_free_heap_size());
}

/* ---------------- app_main ---------------- */
void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);

    ESP_ERROR_CHECK(esp_storage_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 1) Bring up Wi-Fi/netifs first (this avoids NULL deref inside Mesh-Lite)
    wifi_init_root();

    // 2) Mesh-Lite setup (root; no external router required)
    esp_mesh_lite_config_t cfg = ESP_MESH_LITE_DEFAULT_INIT();
    cfg.join_mesh_ignore_router_status    = true;
    cfg.join_mesh_without_configured_wifi = true;  // root works without upstream router
    esp_mesh_lite_init(&cfg);
    esp_mesh_lite_set_allowed_level(1);
    esp_mesh_lite_start();

    // 3) UART for DevKit link (TX=17, RX=18 per Kconfig defaults)
    uart_bridge_init();

    // 4) UDP listener -> UART
    xTaskCreate(udp_rx_task, "udp_rx", 4096, NULL, 5, NULL);

    // 5) Optional system info timer
    TimerHandle_t t = xTimerCreate("sysinfo", 10000/portTICK_PERIOD_MS, pdTRUE, NULL,
                                   print_system_info_timercb);
    xTimerStart(t, 0);
}
