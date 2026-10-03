idf_component_register(
    SRCS "codex_ble.cpp" "gfx.cpp" "board_i2c.cpp" "battery.cpp" "chime.cpp"
    INCLUDE_DIRS "."
    REQUIRES bt json nvs_flash esp_timer efuse esp_hw_support
             esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_usb_serial_jtag
             waveshare__esp32_s3_touch_lcd_1_85B
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
target_add_binary_data(${COMPONENT_LIB} "assets/bg_day.bin" BINARY)
target_add_binary_data(${COMPONENT_LIB} "assets/bg_night.bin" BINARY)
