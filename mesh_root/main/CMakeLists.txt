idf_component_register(
    SRCS "no_router.c"
         "napt_stub.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        lwip
        esp_timer
        esp_http_server
        mesh_lite
        json
        driver      # <- needed for UART/GPIO APIs
)
