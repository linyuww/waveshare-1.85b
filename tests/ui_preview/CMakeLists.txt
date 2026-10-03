cmake_minimum_required(VERSION 3.16)
project(launcher_preview C CXX)
set(CMAKE_CXX_STANDARD 17)
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
add_subdirectory(${ROOT}/firmware/managed_components/lvgl__lvgl lvgl)
add_executable(preview preview.cpp ${ROOT}/firmware/main/app_icons.c ${ROOT}/firmware/main/dark/scr_bg.c ${ROOT}/firmware/main/ui_font.c
${ROOT}/firmware/components/brookesia_core/systems/base/assets/fonts/maison_neue_book/esp_brookesia_font_maison_neue_book_18.c
${ROOT}/firmware/components/brookesia_core/systems/phone/assets/images/large/esp_brookesia_image_large_status_bar_wifi_level3_36_36.c)
target_include_directories(preview PRIVATE ${ROOT}/firmware/main ${ROOT}/firmware/components/codex_micro ${CMAKE_CURRENT_SOURCE_DIR}/stubs)
target_link_libraries(preview PRIVATE lvgl m)

target_compile_options(preview PRIVATE -UNDEBUG)
