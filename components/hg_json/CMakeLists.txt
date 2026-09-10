idf_component_register(SRCS "hg_json_schema.c" "hg_json_cfg.c" "hg_json_mcfg.c" INCLUDE_DIRS "."
    REQUIRES hg_cfg hg_mcfg PRIV_REQUIRES espressif__cjson)
