// mesh_root/main/no_router.c  — ESP-Mesh-Lite v1.0 root with UART mirror

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

static const char *TAG = "mesh_root";

/* v1.0 signature: data, len, from(MAC as 6 bytes) */
static void mesh_data_recv_cb(const uint8_t *data, uint16_t len, const mesh_addr_t *from)
{
    if (!data || !len) return;

    // Mirror the raw packet to UART and add a newline for readability
    uart_bridge_write(data, len);
    const char nl = '\n';
    uart_bridge_write((const uint8_t *)&nl, 1);

    if (from) {
        const uint8_t *m = (const uint8_t *)from;  // mesh_addr_t is a 6-byte array in v1.0
        ESP_LOGI(TAG, "RX %u bytes from %02X:%02X:%02X:%02X:%02X:%02X -> UART",
                 (unsigned)len, m[0], m[1], m[2], m[3], m[4], m[5]);
    } else {
        ESP_LOGI(TAG, "RX %u bytes (sender unknown) -> UART", (unsigned)len);
    }
}

void app_main(void)
{
    // --- IDF bring-up ---
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // --- UART bridge (pins/baud come from Kconfig) ---
    uart_bridge_init();

    // --- Mesh-Lite v1.0: init requires a config pointer ---
    esp_mesh_lite_config_t *cfg = esp_mesh_lite_get_ap_config();   // provided by v1.0
    if (!cfg) {
        ESP_LOGE(TAG, "esp_mesh_lite_get_ap_config() returned NULL");
        abort();
    }
    ESP_ERROR_CHECK(esp_mesh_lite_init(cfg));

    // v1.0 RX callback registration
    esp_mesh_lite_set_receive_cb(mesh_data_recv_cb);

    ESP_ERROR_CHECK(esp_mesh_lite_start());

    ESP_LOGI(TAG, "Root up. Mirroring mesh payloads to UART%d TX=%d @%d",
             CONFIG_UART_BRIDGE_PORT, CONFIG_UART_BRIDGE_TX_PIN, CONFIG_UART_BRIDGE_BAUD);

    // Idle forever; work happens in the RX callback
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
