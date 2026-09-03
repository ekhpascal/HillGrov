idf_component_register(
    SRCS "trial_eval.c" "trial_cmds.c" "ota_trial.c"
    INCLUDE_DIRS "."
    REQUIRES cmd_core app_update nvs_flash esp_timer notify freertos esp_driver_uart
)
