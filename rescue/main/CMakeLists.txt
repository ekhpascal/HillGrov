idf_component_register(SRCS "app_main.c" "rescue_wifi.c" "rescue_http.c" "rescue_pull.c" "ring_fwd.c"
    INCLUDE_DIRS "."
    REQUIRES board rescue_handover app_update esp_http_client esp_wifi esp_http_server
              esp_driver_uart esp_driver_gpio esp_timer nvs_flash
    EMBED_TXTFILES "upload.html")
