// no_router.c (root: UDP -> UART, no UI)
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "lwip/inet.h"

#include "esp_mesh_lite.h"
#include "root_udp.h"
#include "uart_bridge.h"

#ifndef FORCE_ROOT
#define FORCE_ROOT 1
#endif

static const char *TAG = "no_router";

static esp_err_t esp_storage_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

static void app_wifi_set_softap_info(void)
{
    char ssid[33], psw[64];
    uint8_t mac[6];
    esp_err_t er = esp_wifi_get_mac(WIFI_IF_AP, mac);
    if (er != ESP_OK) { ESP_LOGW(TAG, "AP MAC not ready: %s", esp_err_to_name(er)); return; }

#ifdef CONFIG_BRIDGE_SOFTAP_SSID_END_WITH_THE_MAC
    snprintf(ssid, sizeof(ssid), "%.25s_%02x%02x%02x",
             CONFIG_BRIDGE_SOFTAP_SSID, mac[3], mac[4], mac[5]);
#else
    snprintf(ssid, sizeof(ssid), "%.32s", CONFIG_BRIDGE_SOFTAP_SSID);
#endif
    strlcpy(psw, CONFIG_BRIDGE_SOFTAP_PASSWORD, sizeof(psw));
    esp_mesh_lite_set_softap_info(ssid, psw);
}

static void sysinfo_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    uint8_t ch=0; wifi_second_chan_t sc=0; uint8_t sta_mac[6]={0}; wifi_ap_record_t ap={0};
    esp_wifi_get_channel(&ch, &sc);
    esp_wifi_get_mac(ESP_IF_WIFI_STA, sta_mac);
    if (esp_mesh_lite_get_level() > 1) (void)esp_wifi_sta_get_ap_info(&ap);

    ESP_LOGI(TAG, "Ch%u Lvl%d self " MACSTR " parent " MACSTR " rssi %d heap %" PRIu32,
             (unsigned)ch,
             (int)esp_mesh_lite_get_level(),
             MAC2STR(sta_mac), MAC2STR(ap.bssid),
             (ap.rssi != 0 ? ap.rssi : -120),
             (uint32_t)esp_get_free_heap_size());
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);
    ESP_ERROR_CHECK(esp_storage_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_mesh_lite_config_t cfg = ESP_MESH_LITE_DEFAULT_INIT();
    cfg.join_mesh_ignore_router_status = true;
#if FORCE_ROOT
    cfg.join_mesh_without_configured_wifi = false;
    esp_mesh_lite_set_allowed_level(1);
    ESP_LOGI(TAG, "ROOT mode");
#else
    cfg.join_mesh_without_configured_wifi = true;
    esp_mesh_lite_set_disallowed_level(1);
    ESP_LOGI(TAG, "CHILD mode");
#endif

    esp_mesh_lite_init(&cfg);
    esp_mesh_lite_start();

    // set SoftAP label (non-fatal if too early)
    app_wifi_set_softap_info();

    // UART bridge + UDP listener
    uart_bridge_init();
    (void)root_udp_start(3333);

    TimerHandle_t t = xTimerCreate("sysinfo", 10000/portTICK_PERIOD_MS, pdTRUE, NULL, sysinfo_cb);
    xTimerStart(t, 0);
}
