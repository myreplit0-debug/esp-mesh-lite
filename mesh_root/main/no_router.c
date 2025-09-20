/*
 * Mesh-Lite root with UART mirroring
 *
 * - No Wi-Fi / Mesh credentials are changed here.
 * - Force this device to be root in menuconfig:
 *     Component config -> Mesh-Lite -> Only Root  (CONFIG_MESH_LITE_ONLY_ROOT=y)
 * - Any incoming Mesh-Lite JSON whose "type" matches one of the strings
 *   in g_actions[] will be forwarded to UART as a single compact JSON line.
 */

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "cJSON.h"
#include "esp_mesh_lite.h"
#include "uart_bridge.h"

static const char *TAG = "mesh_root";

/* ------------------------------------------------------------------ */
/* Callback that mirrors the *payload JSON* to UART as one line.      */
/* Return NULL (no response JSON).                                    */
/* ------------------------------------------------------------------ */
static cJSON *mirror_to_uart(cJSON *payload, uint32_t seq)
{
    char *raw = cJSON_PrintUnformatted(payload);
    if (raw) {
        uart_bridge_write((const uint8_t *)raw, strlen(raw));
        const char nl = '\n';
        uart_bridge_write((const uint8_t *)&nl, 1);
        cJSON_free(raw);
    }
    ESP_LOGD(TAG, "Mirrored seq=%u to UART", (unsigned)seq);
    return NULL;   // no reply to the sender
}

/* ------------------------------------------------------------------ */
/* Register actions you expect leaves to emit in their "type" field.  */
/* Add/remove strings to fit your project.                            */
/* ------------------------------------------------------------------ */
static const esp_mesh_lite_msg_action_t g_actions[] = {
    { .type = "uart_forward", .rsp_type = NULL, .process = mirror_to_uart },
    { .type = "ble_report",   .rsp_type = NULL, .process = mirror_to_uart },
    { .type = "data",         .rsp_type = NULL, .process = mirror_to_uart },
    /* add more types here if needed */
};

void app_main(void)
{
    /* --- Standard IDF bring-up --- */
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* --- UART bridge (pins/baud come from your Kconfig) --- */
    uart_bridge_init();   // uses CONFIG_UART_BRIDGE_{PORT,TX_PIN,RX_PIN,BAUD}
    ESP_LOGI(TAG, "UART bridge initialised (TX=%d RX=%d baud=%d)",
             CONFIG_UART_BRIDGE_TX_PIN, CONFIG_UART_BRIDGE_RX_PIN, CONFIG_UART_BRIDGE_BAUD);

    /* --- Mesh-Lite init: get config from library and pass it back as-is --- */
    esp_mesh_lite_config_t *cfg = esp_mesh_lite_get_config();
    // DO NOT set removed/unknown fields here (e.g., no cfg->only_root).
    ESP_ERROR_CHECK(esp_mesh_lite_init(cfg));

    /* Register actions BEFORE start */
    esp_mesh_lite_msg_action_list_register(g_actions);

    /* Start mesh */
    ESP_ERROR_CHECK(esp_mesh_lite_start());
    ESP_LOGI(TAG, "Mesh-Lite root started; forwarding selected messages to UART");

    /* Optional heartbeat so you know it’s alive */
    const int64_t period_us = 3000000; // 3 s
    int64_t next = esp_timer_get_time() + period_us;
    while (true) {
        if (esp_timer_get_time() >= next) {
            ESP_LOGI(TAG, "alive; waiting for mesh messages…");
            next += period_us;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
