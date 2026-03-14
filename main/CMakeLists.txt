idf_component_register(
    SRCS "Labfinal.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi
             esp_event
             nvs_flash
             esp_http_client
             esp_http_server
             mbedtls
             esp_timer
             esp32-camera
)