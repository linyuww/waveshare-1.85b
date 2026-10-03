idf_component_register(
    SRCS "main.cpp" "system_service.cpp" "bluetooth_service.cpp" "codex_micro_app.cpp" "launcher_apps.cpp" "app_icons.c"
         "ui_font.c" "dark/scr_bg.c" "fitness_app.cpp" "fitness_model.cpp"
         "fitness_storage.cpp" "fitness_view.cpp" "fitness_assets.c"
    INCLUDE_DIRS "."
    REQUIRES brookesia_core waveshare__esp32_s3_touch_lcd_1_85B codex_micro
             esp_wifi esp_netif esp_event nvs_flash esp_http_client mbedtls esp_timer esp_app_format
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
