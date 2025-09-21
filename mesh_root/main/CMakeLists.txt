idf_component_register(
    SRCS "no_router.c"
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
        driver          # <-- REQUIRED for UART
)

# Stop warnings killing the build
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error
    -Wno-unused-function
    -Wno-implicit-function-declaration
)
