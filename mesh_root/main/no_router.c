/* mesh_root/main/no_router.c
 *
 * Root node for ESP-Mesh-Lite (no-router example, extended):
 * - Initializes UART1 (via uart_bridge.c) with TX on GPIO17.
 * - Registers a cJSON action so that any mesh message whose
 *   JSON contains {"type":"<ACTION_TYPE>"} is mirrored to UART.
 *
 * Change ACTION_TYPE to match what your leaves already send.
 * If leaves already send {"type":"ble_report"} or {"type":"data"},
 * just set ACTION_TYPE to that string and rebuild the root.
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

/* === Set this to the existing 'type' your leaves already send === */
#define ACTION_TYPE "uart_forward"   // e.g. "ble_report", "data", "ice_out", etc.

/* ---- Action callback: mirror payload JSON to UART as one line ---- */
static cJSON *on_action_forward(cJSON *payload, uint32_t seq)
{
    /* If you only want a field (e.g., "data"), uncomment below:
       cJSON *d = cJSON_GetObjectItemCaseSensitive(payload, "data");
       if (cJSON_IsString(d) && d->valuestring) {
           const char *s = d->valuestring;
           uart_bridge_write((const uint8_t*)s, strlen(s));
           const char nl = '\n';
           uart_bridge_write((const uint8_t*)&nl, 1);
           return NULL;
       }
    */

    /* Default: forward the entire JSON compactly */
    char *raw = cJSON_PrintUnformatted(payload);
    if (raw) {
        uart_bridge_write((const uint8_t *)raw, strlen(raw));
        const char nl = '\n';
        uart_bridge_write((const uint8_t *)&nl, 1);
        cJSON_free(raw);
    }

    ESP_LOGD(TAG, "mirrored action '%s' seq=%u", ACTION_TYPE, (unsigned)seq);
    return NULL;  // no response JSON
}

/* ---- Register exactly one action name -> callback ---- */
static const esp_mesh_lite_msg_action_t g_actions[] = {
    {
        .type     = ACTION_TYPE,   // must match the leaf's JSON "type"
        .rsp_type = NULL,          // no response
        .process  = on_action_forward
    },
};

void app_main(void)
{
    /* Standard IDF bring-up */
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Our UART bridge (TX=17 by default via Kconfig/sdkconfig.defaults) */
    uart_bridge_init();

    /* Mesh-Lite bring-up */
    esp_mesh_lite_config_t cfg = ESP_MESH_LITE_DEFAULT_INIT();
    cfg.only_root = true;                 // keep this device as the root
    ESP_ERROR_CHECK(esp_mesh_lite_init(&cfg));

    /* Register the action list BEFORE starting the mesh */
    ESP_ERROR_CHECK(esp_mesh_lite_msg_action_list_register(g_actions));

    ESP_ERROR_CHECK(esp_mesh_lite_start());

    ESP_LOGI(TAG, "Root up. Forwarding JSON where type=\"%s\" to UART TX=%d @%d",
             ACTION_TYPE, CONFIG_UART_BRIDGE_TX_PIN, CONFIG_UART_BRIDGE_BAUD);

    /* Optional heartbeat */
    const int64_t every_us = 3 * 1000 * 1000;
    int64_t next = esp_timer_get_time() + every_us;
    while (1) {
        if (esp_timer_get_time() >= next) {
            ESP_LOGI(TAG, "alive; waiting for mesh messages…");
            next += every_us;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
