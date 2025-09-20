idf_component_register(
    SRCS
        "no_router.c"
        "uart_bridge.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        driver
        esp_timer
        mesh_lite
        json
)
