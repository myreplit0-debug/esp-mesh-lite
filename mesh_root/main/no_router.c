/* mesh_root/main/no_router.c — Root with UART mirror (Mesh-Lite v1.0.2) */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_timer.h"

#include "cJSON.h"
#include "esp_mesh_lite.h"
#include "uart_bridge.h"

static const char *TAG = "mesh_root";

/* change to match the leaf’s JSON "type" */
#define ACTION_TYPE "uart_forward"

/* mirror payload JSON to UART as one line */
static cJSON *on_action_forward(cJSON *payload, uint32_t seq)
{
    char *raw = cJSON_PrintUnformatted(payload);
    if (raw) {
        uart_bridge_write((const uint8_t *)raw, strlen(raw));
        const char nl = '\n';
        uart_bridge_write((const uint8_t *)&nl, 1);
        cJSON_free(raw);
    }
    ESP_LOGD(TAG, "mirrored type='%s' seq=%u", ACTION_TYPE, (unsigned)seq);
    return NULL;  // no response JSON
}

/* action table */
static const esp_mesh_lite_msg_action_t g_actions[] = {
    { .type = ACTION_TYPE, .rsp_type = NULL, .process = on_action_forward },
};

void app_main(void)
{
    /* IDF bring-up */
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();

    /* UART */
    uart_bridge_init();

    /* Mesh-Lite (v1.0.2: init/start are void, no args) */
    esp_mesh_lite_init();
    esp_mesh_lite_msg_action_list_register(g_actions);
    esp_mesh_lite_start();

    ESP_LOGI(TAG, "Root up. Forwarding JSON type=\"%s\" to UART TX=%d @%d",
             ACTION_TYPE, CONFIG_UART_BRIDGE_TX_PIN, CONFIG_UART_BRIDGE_BAUD);

    /* heartbeat */
    int64_t next = esp_timer_get_time() + 3LL*1000*1000;
    while (1) {
        if (esp_timer_get_time() >= next) {
            ESP_LOGI(TAG, "alive; waiting for mesh messages…");
            next += 3LL*1000*1000;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
