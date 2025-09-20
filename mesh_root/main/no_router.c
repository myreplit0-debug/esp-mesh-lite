/*  mesh_root — no_router.c  (UART forwarder)
 *
 *  Root receives Mesh-Lite app messages of type 0x31 from leaves
 *  and mirrors the payload bytes out UART1 TX=17 to another ESP32.
 *
 *  Requires:
 *    - uart_bridge.c/.h in this component (already in your repo)
 *    - sdkconfig.defaults sets CONFIG_MESH_LITE_ONLY_ROOT=y (or cfg.only_root=true below)
 */

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_timer.h"

#include "esp_mesh_lite.h"
#include "uart_bridge.h"

static const char *TAG = "mesh_root";

/* ---------------- App message used for forwarding ---------------- */
#define MSG_UART_FORWARD 0x31  // leaves must send with this type

static esp_err_t on_msg_uart_forward(const uint8_t *src_mac,
                                     const uint8_t *payload, uint16_t len,
                                     void *usr_ctx)
{
    // Write raw payload to UART, then a newline for line readers
    uart_bridge_write(payload, len);
    const char nl = '\n';
    uart_bridge_write((const uint8_t *)&nl, 1);

    ESP_LOGD(TAG, "Forwarded %u bytes from %02X:%02X:%02X:%02X:%02X:%02X",
             (unsigned)len,
             src_mac[0], src_mac[1], src_mac[2],
             src_mac[3], src_mac[4], src_mac[5]);
    return ESP_OK;
}

static esp_mesh_lite_msg_action_t g_actions[] = {
    {
        .type      = MSG_UART_FORWARD,
        .desc      = "Forward leaf payloads to UART1",
        .priv_data = NULL,
        .recv_cb   = on_msg_uart_forward,
        .timeout   = 3000,  // only used if you expect replies
    },
};

/* ----------------------------- app_main --------------------------- */
void app_main(void)
{
    // Basic IDF bring-up
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Our UART bridge (TX=17 by default; see sdkconfig/Kconfig)
    uart_bridge_init();

    // Mesh-Lite configuration
    esp_mesh_lite_config_t cfg = ESP_MESH_LITE_DEFAULT_INIT();
    cfg.only_root = true;  // keep this device as the root

    ESP_ERROR_CHECK(esp_mesh_lite_init(&cfg));

    // Register message actions BEFORE start
    ESP_ERROR_CHECK(esp_mesh_lite_register_msg_action_list(
        g_actions, sizeof(g_actions) / sizeof(g_actions[0])));

    ESP_ERROR_CHECK(esp_mesh_lite_start());

    ESP_LOGI(TAG, "Root is up. Mirroring MSG 0x%02X to UART TX=%d @%d baud",
             MSG_UART_FORWARD,
             CONFIG_UART_BRIDGE_TX_PIN,
             CONFIG_UART_BRIDGE_BAUD);

    // Optional heartbeat
    const int64_t period_us = 3 * 1000 * 1000;
    int64_t next = esp_timer_get_time() + period_us;
    while (1) {
        int64_t now = esp_timer_get_time();
        if (now >= next) {
            ESP_LOGI(TAG, "alive; waiting for leaf messages…");
            next += period_us;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
