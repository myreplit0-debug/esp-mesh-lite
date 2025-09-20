idf_component_register(
    SRCS
        "no_router.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_event
        nvs_flash
        lwip
        esp_timer
        esp_http_server
        esp_mesh_lite
        json
)
