/* mesh_root/main/no_router.c — Root with UART mirror (stable)
 *
 * Mirrors any Mesh-Lite JSON whose {"type":"<ACTION_TYPE>"} matches
 * to UART1 TX=17 (via uart_bridge.c). Mesh SSID/password/channel are unchanged.
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

/* Change to the 'type' your leaves already send */
#define ACTION_TYPE "uart_forward"

/* ---- Action callback: mirror payload JSON to UART as one line ---- */
static cJSON *on_action_forward(cJSON *payload, uint32_t seq)
{
    // If you only want a specific field, you can use this pattern:
    /*
    cJSON *d = cJSON_GetObjectItemCaseSensitive(payload, "data");
    if (cJSON_IsString(d) && d->valuestring) {
        const char *s = d->valuestring;
        uart_bridge_write((const uint8_t*)s, strlen(s));
        const char nl = '\n';
        uart_bridge_write((const uint8_t*)&nl, 1);
        return NULL;
    }
    */

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

/* ---- Action list MUST be NULL-terminated in this Mesh-Lite version ---- */
static const esp_mesh_lite_msg_action_t g_actions[] = {
    { .type = ACTION_TYPE, .rsp_type = NULL, .process = on_action_forward },
    { .type = NULL,        .rsp_type = NULL, .process = NULL }   // terminator
};

void app_main(void)
{
    // Standard IDF bring-up
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create the Wi-Fi netifs Mesh-Lite expects (STA and AP) BEFORE init
    // (No extra components required; these are IDF helpers.)
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    // UART1 (TX pin via Kconfig/sdkconfig.defaults; default TX=17)
    uart_bridge_init();

    // Mesh-Lite init/start — these are 'void' in your build
    esp_mesh_lite_config_t cfg = ESP_MESH_LITE_DEFAULT_INIT();
    esp_mesh_lite_init(&cfg);

    // Register actions BEFORE start (also void)
    esp_mesh_lite_msg_action_list_register(g_actions);

    esp_mesh_lite_start();

    ESP_LOGI(TAG, "Root up. Forwarding JSON where type=\"%s\" to UART TX=%d @%d",
             ACTION_TYPE, CONFIG_UART_BRIDGE_TX_PIN, CONFIG_UART_BRIDGE_BAUD);

    // Optional heartbeat
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
