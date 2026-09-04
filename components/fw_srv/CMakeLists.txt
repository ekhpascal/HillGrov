idf_component_register(
    SRCS "fw_srv.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server esp_partition hg_blob
)
