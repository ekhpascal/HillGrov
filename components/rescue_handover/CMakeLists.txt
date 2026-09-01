idf_component_register(
    SRCS "rescue_handover.c" "rescue_handover_nvs.c"
    INCLUDE_DIRS "."
    REQUIRES hg_blob nvs_flash
)
