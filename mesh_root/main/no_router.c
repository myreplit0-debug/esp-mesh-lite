// mesh_root/main/no_router.c
//
// Root: receive UDP :3333 from leaves -> forward to UART (TX=17)
// No mesh callbacks, no JSON actions, just a UDP socket + UART bridge.

#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "uart_bridge.h"

#define UDP_PORT 3333
static const char *TAG = "mesh_root";

static void udp_task(void *pvParameters)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(UDP_PORT);

    if (bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Listening on UDP port %d", UDP_PORT);

    uint8_t rxbuf[256];
    while (1) {
        int len = recvfrom(sock, rxbuf, sizeof(rxbuf)-1, 0, NULL, 0);
        if (len > 0) {
            uart_bridge_write(rxbuf, len);
            const char nl = '\n';
            uart_bridge_write((const uint8_t *)&nl, 1);
            ESP_LOGI(TAG, "UDP->UART: %d bytes", len);
        }
    }

    close(sock);
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    uart_bridge_init();

    xTaskCreate(udp_task, "udp_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Root up. Mirroring UDP:%d to UART%d TX=%d @%d",
             UDP_PORT, CONFIG_UART_BRIDGE_PORT,
             CONFIG_UART_BRIDGE_TX_PIN, CONFIG_UART_BRIDGE_BAUD);
}
