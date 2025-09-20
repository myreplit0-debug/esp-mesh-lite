/* mesh_root/main/no_router.c — Root with UDP->UART mirror (matches your leaf)
 *
 * Leaf sends UART bytes as UDP datagrams to port 3333 over Mesh-Lite.
 * Root listens on UDP :3333 and mirrors each datagram to UART1 TX=17.
 * Nothing about SSID/password/channel is changed here.
 */

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "esp_mesh_lite.h"
#include "uart_bridge.h"

static const char *TAG = "mesh_root";
#define UDP_PORT 3333
#define RX_BUF   1500

static void udp_rx_task(void *arg)
{
    int sock = -1;

    // Create UDP socket
    while (sock < 0) {
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGW(TAG, "socket() failed, retrying...");
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

    // Bind to :3333 on any interface (Mesh-Lite sets up netifs)
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(UDP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    while (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGW(TAG, "bind(:%d) failed, retrying...", UDP_PORT);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    ESP_LOGI(TAG, "UDP listener bound on :%d", UDP_PORT);

    uint8_t buf[RX_BUF];

    for (;;) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &slen);
        if (n > 0) {
            // Mirror raw bytes to UART1 (TX=CONFIG_UART_BRIDGE_TX_PIN)
            uart_bridge_write(buf, (size_t)n);

            // Optional: add newline if you want line-oriented output
            // const char nl = '\n';
            // uart_bridge_write((const uint8_t *)&nl, 1);
        } else {
            // yield a bit on errors or timeouts
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    // (never reached)
    // close(sock);
    // vTaskDelete(NULL);
}

void app_main(void)
{
    // Standard IDF bring-up
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Bring up UART1 bridge (TX defaults from Kconfig/sdkconfig.defaults)
    uart_bridge_init();
    ESP_LOGI(TAG, "UART bridge ready (TX=%d RX=%d @%d)",
             CONFIG_UART_BRIDGE_TX_PIN, CONFIG_UART_BRIDGE_RX_PIN, CONFIG_UART_BRIDGE_BAUD);

    // Start Mesh-Lite (v1.0.x API is void init/start)
    esp_mesh_lite_init();
    esp_mesh_lite_start();
    ESP_LOGI(TAG, "Mesh-Lite started; waiting for UDP %d...", UDP_PORT);

    // Start UDP receiver
    xTaskCreate(udp_rx_task, "udp_rx_3333", 4096, NULL, 5, NULL);

    // Heartbeat (optional)
    int64_t next = esp_timer_get_time() + 3000000LL;
    while (true) {
        if (esp_timer_get_time() >= next) {
            ESP_LOGI(TAG, "alive; mesh root listening on :%d", UDP_PORT);
            next += 3000000LL;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
