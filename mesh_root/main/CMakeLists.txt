idf_component_register(
    SRCS
        "no_router.c"
        "root_udp.c"
        "uart_bridge.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        driver
        esp_timer
        mesh_lite         # <-- this was esp_mesh_lite; change to mesh_lite
)
