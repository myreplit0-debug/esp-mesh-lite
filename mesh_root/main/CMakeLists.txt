idf_component_register(
    SRCS "no_router.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_event
        nvs_flash
        lwip
        esp_timer
        esp_http_server
        driver          # for UART
        mesh_lite       # <<< local component name
        json
)

# (Optional) keep warnings as warnings inside this component
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error)
