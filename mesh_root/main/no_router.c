// mesh_root/main/no_router.c  (v1.0-friendly, action-based UART mirror)

#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "esp_mesh_lite.h"
#include "uart_bridge.h"
#include "cJSON.h"

static const char *TAG = "mesh_root";

// Change this to the action your leaves already send.
// e.g. "ble_report", "data", "uart_forward", etc.
#define ACTION_TYPE "uart_forward"

/* Action handler: mirror payload JSON to UART as one line */
static cJSON *on_action_forward(cJSON *payload, uint32_t seq)
{
    // Forward the whole JSON payload, compact, + newline
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

/* Register exactly one action: name -> callback */
static const esp_mesh_lite_msg_action_t g_actions[] = {
    {
        .type     = ACTION_TYPE,   // must match leaf JSON "type"
        .rsp_type = NULL,          // no reply
        .process  = on_action_forward
    },
};

void app_main(void)
{
    // Standard IDF bring-up
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Our UART bridge (pins/baud from sdkconfig/Kconfig)
    uart_bridge_init();

    // Mesh-Lite init using the v1.0 helper
    esp_mesh_lite_config_t *cfg = esp_mesh_lite_get_ap_config();
    ESP_ERROR_CHECK(esp_mesh_lite_init(cfg));

    // IMPORTANT: register actions before start (v1.0 API)
    esp_mesh_lite_msg_action_list_register(g_actions);

    ESP_ERROR_CHECK(esp_mesh_lite_start());

    ESP_LOGI(TAG, "Root ready. Mirroring action type \"%s\" to UART%d TX=%d @%d",
             ACTION_TYPE, CONFIG_UART_BRIDGE_PORT,
             CONFIG_UART_BRIDGE_TX_PIN, CONFIG_UART_BRIDGE_BAUD);

    // Idle loop
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
