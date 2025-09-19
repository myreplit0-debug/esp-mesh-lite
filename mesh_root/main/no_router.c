// Root: receive UDP :3333 from leaves -> forward to UART (TX=17, RX=16)

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
#include "sdkconfig.h"   // Kconfig symbols if present

#ifndef CONFIG_MESH_CHANNEL
#define CONFIG_MESH_CHANNEL 6
#endif
#ifndef CONFIG_BRIDGE_SOFTAP_SSID
#define CONFIG_BRIDGE_SOFTAP_SSID     "meshmesh"
#endif
#ifndef CONFIG_BRIDGE_SOFTAP_PASSWORD
#define CONFIG_BRIDGE_SOFTAP_PASSWORD "12345678"
#endif

#define UDP_PORT    3333
#define FORCE_ROOT  1

static const char *TAG = "mesh_root";

/* ---------- NVS ---------- */
static esp_err_t esp_storage_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

/* ---------- UDP -> UART ---------- */
static void udp_rx_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { ESP_LOGE(TAG, "socket() failed"); vTaskDelete(NULL); return; }

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

    ESP_LOGI(TAG, "UDP :%d listening; forwarding to UART...", UDP_PORT);

    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in from = {0};
        socklen_t fromlen = sizeof(from);
        int r = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
        if (r > 0) {
            uart_bridge_write(buf, (size_t)r);
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

/* ---------- Optional heartbeat ---------- */
static void print_system_info_timercb(TimerHandle_t tmr)
{
    (void)tmr;
    uint8_t ch = 0; wifi_second_chan_t sc = 0; esp_wifi_get_channel(&ch, &sc);
    uint8_t mac[6] = {0}; esp_wifi_get_mac(ESP_IF_WIFI_STA, mac);
    ESP_LOGI(TAG, "Ch%u Level %d Self " MACSTR " Heap %u",
             ch, esp_mesh_lite_get_level(), MAC2STR(mac),
             (unsigned)esp_get_free_heap_size());
}

/* ---------- app_main ---------- */
void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);

    ESP_ERROR_CHECK(esp_storage_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Mesh-Lite setup (ROOT)
    esp_mesh_lite_config_t cfg = ESP_MESH_LITE_DEFAULT_INIT();
    cfg.join_mesh_ignore_router_status = true;
    cfg.join_mesh_without_configured_wifi = true;   // <-- allow join even if STA creds are empty

    esp_mesh_lite_init(&cfg);
    esp_mesh_lite_set_allowed_level(1);
    esp_mesh_lite_start();

    // Make sure SoftAP is defined so children can find us
    esp_mesh_lite_set_softap_info(CONFIG_BRIDGE_SOFTAP_SSID, CONFIG_BRIDGE_SOFTAP_PASSWORD);

    // UART1 on TX=17, RX=16 for the DevKit
    uart_bridge_init();  // should configure UART_NUM_1, tx=17, rx=16

    // Start UDP receiver -> UART forwarder
    xTaskCreate(udp_rx_task, "udp_rx", 4096, NULL, 5, NULL);

    // Optional heartbeat
    TimerHandle_t t = xTimerCreate("sysinfo", 10000/portTICK_PERIOD_MS, pdTRUE, NULL,
                                   print_system_info_timercb);
    xTimerStart(t, 0);

    ESP_LOGI(TAG, "Root ready. mesh_ch=%d UDP=%d UART TX=17 RX=16",
             CONFIG_MESH_CHANNEL, UDP_PORT);
}
