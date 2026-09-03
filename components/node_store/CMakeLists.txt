idf_component_register(
    SRCS "ztab.c" "node_store_nvs.c"
    INCLUDE_DIRS "."
    REQUIRES hg_blob ring_proto nvs_flash
)
