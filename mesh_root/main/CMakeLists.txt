idf_component_register(
    SRCS
        "no_router.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        esp_bridge        # <— needed for esp_bridge_* APIs
        esp_mesh_lite
        esp_wifi
        esp_netif         # sockets/netif
        lwip              # UDP sockets
        nvs_flash
        esp_event
        esp_timer
        driver
        json
)
