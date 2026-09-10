idf_component_register(SRCS "alarm_mgr.c" "alarm_mgr_json.c" INCLUDE_DIRS "."
    REQUIRES notify PRIV_REQUIRES espressif__cjson)
