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
        mesh_lite
        json
)

# Disable treating warnings as errors, and suppress specific common warnings
target_compile_options(${COMPONENT_LIB} PRIVATE 
    -Wno-error                        # Do not treat any warnings as errors3
    -Wno-unused-function              # Ignore unused-function warnings 
    -Wno-implicit-function-declaration# Ignore missing declaration warnings
)
