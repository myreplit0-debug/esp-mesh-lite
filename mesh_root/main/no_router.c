// mesh_root/main/no_router.c  (ESP-Mesh-Lite v1.0 compatible)
// Root node: receive raw mesh data and mirror it to UART (TX pin from Kconfig)

#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "esp_mesh_lite.h"   // v1.0 API
#include "uart_bridge.h"     // your small UART helper

static const char *TAG = "mesh_root";

/* Mesh RX callback (signature must match v1.0 exactly) */
static void mesh_data_recv_cb(const uint8_t *data, uint16_t len, const mesh_addr_t *from)
{
    if (!data || !len) return;

    // Mirror bytes to external UART, then newline for readability
    uart_bridge_write(data, len);
    const char nl = '\n';
    uart_bridge_write((const uint8_t *)&nl, 1);

    ESP_LOGI(TAG, "RX %u bytes from %02X:%02X:%02X:%02X:%02X:%02X -> UART",
             (unsigned)len,
             from->addr[0], from->addr[1], from->addr[2],
             from->addr[3], from->addr[4], from->addr[5]);
}

void app_main(void)
{
    // --- IDF bring-up ---
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // --- UART out (115200 by default; pins from Kconfig) ---
    uart_bridge_init();

    // --- Mesh-Lite bring-up (v1.0 has void init/start) ---
    ESP_ERROR_CHECK(esp_mesh_lite_init());

    // Register our RX callback BEFORE start (v1.0 name)
    esp_mesh_lite_register_data_cb(mesh_data_recv_cb);

    ESP_ERROR_CHECK(esp_mesh_lite_start());

    ESP_LOGI(TAG,
        "Root is up. Mirroring mesh payloads to UART%d TX=%d @%d baud",
        CONFIG_UART_BRIDGE_PORT, CONFIG_UART_BRIDGE_TX_PIN, CONFIG_UART_BRIDGE_BAUD);

    // Nothing else to do; everything happens in the callback.
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
