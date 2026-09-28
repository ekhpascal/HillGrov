# components/panel_ui: the Master v2 7" LVGL face. Sources ONLY for the P4
# master: components/ is in EXTRA_COMPONENT_DIRS for master, zone and rescue
# (and will be for rescue_p4), and the panel must never land in any of them
# (recovery design decision 4: no panel in rescue). Two gates, both needed:
#   - CMAKE_PROJECT_NAME keeps it out of zone/rescue/rescue_p4;
#   - IDF_TARGET keeps it out of the ESP32 (DevKitC) master.
# SRCS gating is safe in this late pass (wifi_mgr :25-28, http_srv :11-16).
# REQUIRES/PRIV_REQUIRES are NOT gated -- IDF evaluates them in an early pass
# where a CONFIG_/project-name gate silently comes out empty
# (cp_ota/CMakeLists.txt:17-31, wifi_mgr/CMakeLists.txt:14-19) -- so they
# list only components present in every app build. The P4-only managed
# components (BSP, lvgl, adapter, touch) attach through
# idf_component_optional_requires() after registration, the cp_ota idiom:
# it checks BUILD_COMPONENTS and is a no-op wherever they are absent.
set(PANEL_SRCS "")
idf_build_get_property(panel_target IDF_TARGET)
if("${CMAKE_PROJECT_NAME}" STREQUAL "hillgrow_master" AND "${panel_target}" STREQUAL "esp32p4")
    set(PANEL_SRCS "panel_ui.c" "panel_hw.c" "panel_lock.c" "pnl_touch.c" "pnl_theme.c" "scr_diag.c" "pnl_worker.c"
                   "pnl_time.c" "pnl_fmt.c" "pnl_home.c" "pnl_poll.c" "scr_shell.c" "scr_dashboard.c"
                   "font_clock_180.c" "scr_home.c" "scr_audio.c" "scr_zone.c" "scr_alarms.c")
    list(APPEND PANEL_SRCS "pcfg_gen.c" "pcfg_pres.c")      # Task 17: the config-widget generator (pure)
    list(APPEND PANEL_SRCS "pcfg_edit.c" "pnl_msg.c")       # Task 18: edit model, refusal texts (pure)
    list(APPEND PANEL_SRCS "pnl_input.c" "wdg_field.c" "wdg_keyboard.c")   # Task 19: field widgets, keyboard
    list(APPEND PANEL_SRCS "scr_config.c" "cfg_zone.c")     # Task 20: the generated config editor (zones)
endif()

idf_component_register(SRCS ${PANEL_SRCS} INCLUDE_DIRS "."
    REQUIRES panel_svc hg_cfg hg_mcfg ring_proto cmd_core
    PRIV_REQUIRES cmd_task node_mgr alarm_mgr wifi_mgr time_svc app_common state_snap notify board hg_blob
                  nvs_flash esp_timer esp_driver_ledc fatfs sdmmc esp_driver_sdmmc esp_app_format)
# every name above exists in every app build (the early requirements pass evaluates this file for zone/rescue too);
# panel_ui never requires http_srv -- the web's quarantine count reaches it through psvc_state (Task 9)
if(PANEL_SRCS)
    idf_component_optional_requires(PRIVATE waveshare__esp32_p4_wifi6_touch_lcd_7b lvgl__lvgl
                                            espressif__esp_lvgl_adapter espressif__esp_lcd_touch)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
endif()
