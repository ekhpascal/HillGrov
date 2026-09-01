idf_component_register(SRCS "hg_store.c" INCLUDE_DIRS "."
    REQUIRES nvs_flash hg_blob hg_cfg hg_model freertos esp_system)
