/* mesh_root/main/no_router.c  —  Root with UART mirror
 *
 * Root receives Mesh-Lite JSON messages and mirrors those whose
 * {"type":"<ACTION_TYPE>"} matches to UART1 TX=17 (via uart_bridge.c).
 *
 * Nothing in the mesh SSID/password/channel is changed.
 * If your leaves already send JSON with a "type" field, just
 * set ACTION_TYPE to that same string. No leaf edits needed.
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

/* === Change this to the 'type' your leaves already send === */
#define ACTION_TYPE "uart_forward"   // e.g. "ble_report", "data", etc.

/* ---- Action callback: mirror payload JSON to UART as one line ---- */
static cJSON *on_action_forward(cJSON *payload, uint32_t seq)
{
    /* If you only want a specific field:
       cJSON *d = cJSON_GetObjectItemCaseSensitive(payload, "data");
       if (cJSON_IsString(d) && d->valuestring) {
           const char *s = d->valuestring;
           uart_bridge_write((const uint8_t*)s, strlen(s));
           const char nl = '\n';
           uart_bridge_write((const uint8_t*)&nl, 1);
           return NULL;
       }
    */

    // Default: forward the entire JSON compactly
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

/* ---- Register the action list (name -> callback) ---- */
static const esp_mesh_lite_msg_action_t g_actions[] = {
    {
        .type     = ACTION_TYPE,   // must match leaf JSON "type"
        .rsp_type = NULL,          // no reply
        .process  = on_action_forward
    },
};

void app_main(void)
{
    /* Standard IDF bring-up */
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* UART bridge (TX defaults to GPIO17 via Kconfig/sdkconfig.defaults) */
    uart_bridge_init();

    /* Mesh-Lite bring-up
       NOTE: in your release these are 'void' and take no args. */
    esp_mesh_lite_init();

    /* If you want to force this device to always be root,
       set it in Kconfig: CONFIG_MESH_LITE_ONLY_ROOT=y
       (no struct member for 'only_root' in this version). */

    /* Register actions BEFORE start */
    esp_mesh_lite_msg_action_list_register(g_actions);

    esp_mesh_lite_start();

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
