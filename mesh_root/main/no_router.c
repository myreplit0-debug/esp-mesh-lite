/* mesh_root/main/no_router.c  — Minimal root + optional UART mirror
 *
 * Nothing about SSID/password/channel is changed.
 * Works with ESP-Mesh-Lite v1.0.x (IDF v5).
 *
 * If you want to mirror selected mesh actions to UART, set
 *   #define ENABLE_UART_FORWARD 1
 * and set ACTION_TYPE to match the "type" your leaves already send.
 */

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "esp_wifi.h"
#include "cJSON.h"
#include "esp_mesh_lite.h"

#include "uart_bridge.h"

#define ENABLE_UART_FORWARD 0           // start MINIMAL (0). set to 1 after it boots cleanly.
#define ACTION_TYPE         "uart_forward"

static const char *TAG = "mesh_root";

/* ========== OPTIONAL UART forward action ========== */
#if ENABLE_UART_FORWARD
/* Mesh-Lite action callback signature for v1.0.x */
static cJSON *on_action_forward(cJSON *payload, uint32_t seq)
{
    // Forward the entire JSON as a single line to the UART bridge
    char *raw = cJSON_PrintUnformatted(payload);
    if (raw) {
        uart_bridge_write((const uint8_t *)raw, strlen(raw));
        const char nl = '\n';
        uart_bridge_write((const uint8_t *)&nl, 1);
        cJSON_free(raw);
    }
    ESP_LOGD(TAG, "mirrored action '%s' seq=%u", ACTION_TYPE, (unsigned)seq);
    return NULL; // no response JSON
}

/* action table */
static const esp_mesh_lite_msg_action_t g_actions[] = {
    {
        .type     = ACTION_TYPE,   // must match leaf JSON "type"
        .rsp_type = NULL,          // we don't send a response
        .process  = on_action_forward,
    },
};
#endif
/* ================================================== */

void app_main(void)
{
    // --- mandatory IDF bring-up ---
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // --- optional: bring up UART bridge early so logging is visible on that port too ---
    uart_bridge_init();

    // --- Mesh-Lite bring-up (v1.0.x) ---
    // NOTE: v1.0.x uses void esp_mesh_lite_init(void) / void esp_mesh_lite_start(void)
    esp_mesh_lite_init();

#if ENABLE_UART_FORWARD
    // Register actions BEFORE start (single-argument API in v1.0.x)
    esp_mesh_lite_msg_action_list_register(g_actions);
#endif

    esp_mesh_lite_start();

    ESP_LOGI(TAG, "Root up. UART TX=%d RX=%d @%d. Forward=%s, type=\"%s\"",
             CONFIG_UART_BRIDGE_TX_PIN, CONFIG_UART_BRIDGE_RX_PIN, CONFIG_UART_BRIDGE_BAUD,
#if ENABLE_UART_FORWARD
             "ON",
#else
             "OFF",
#endif
             ACTION_TYPE);

    // simple heartbeat so the task stays alive
    const int64_t every_us = 3000000; // 3 s
    int64_t next = esp_timer_get_time() + every_us;
    while (true) {
        if (esp_timer_get_time() >= next) {
            ESP_LOGI(TAG, "alive; waiting for mesh messages…");
            next += every_us;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
