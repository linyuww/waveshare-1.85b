cmake_minimum_required(VERSION 3.16)
project(launcher_preview C CXX)
set(CMAKE_CXX_STANDARD 17)
set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
file(TO_CMAKE_PATH "$ENV{IDF_PATH}" IDF_ROOT)
add_subdirectory(${ROOT}/firmware/managed_components/lvgl__lvgl lvgl)
add_executable(preview preview.cpp ${ROOT}/firmware/main/app_icons.c ${ROOT}/firmware/main/dark/scr_bg.c ${ROOT}/firmware/main/ui_font.c
${ROOT}/firmware/components/brookesia_core/systems/base/assets/fonts/maison_neue_book/esp_brookesia_font_maison_neue_book_18.c
${ROOT}/firmware/components/brookesia_core/systems/phone/assets/images/large/esp_brookesia_image_large_status_bar_wifi_level3_36_36.c)
target_include_directories(preview PRIVATE ${ROOT}/firmware/main ${ROOT}/firmware/components/codex_micro ${CMAKE_CURRENT_SOURCE_DIR}/stubs)
target_link_libraries(preview PRIVATE lvgl m)
add_executable(fitness_preview fitness_preview.cpp
    ${ROOT}/firmware/main/fitness_model.cpp ${ROOT}/firmware/main/fitness_view.cpp
    ${ROOT}/firmware/main/fitness_assets.c ${ROOT}/firmware/main/ui_font.c)
target_include_directories(fitness_preview PRIVATE ${ROOT}/firmware/main)
target_link_libraries(fitness_preview PRIVATE lvgl m)
target_compile_options(fitness_preview PRIVATE -UNDEBUG -Wall -Wextra)

target_compile_options(preview PRIVATE -UNDEBUG)
add_executable(audio_preview ../audio/audio_ui_preview.cpp
    ${ROOT}/firmware/main/audio_apps.cpp ${ROOT}/firmware/main/app_icons.c ${ROOT}/firmware/main/ui_font.c)
target_include_directories(audio_preview PRIVATE ${ROOT}/tests/audio/stubs ${ROOT}/firmware/main
    ${ROOT}/firmware/components/shared_audio ${IDF_ROOT}/components/json/cJSON)
target_link_libraries(audio_preview PRIVATE lvgl m)
target_compile_options(audio_preview PRIVATE -Wall -Wextra -Werror -UNDEBUG)
foreach(theme day night)
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/bg_${theme}.o
        COMMAND ${CMAKE_LINKER} -r -b binary bg_${theme}.bin -o ${CMAKE_CURRENT_BINARY_DIR}/bg_${theme}.o
        WORKING_DIRECTORY ${ROOT}/firmware/components/codex_micro/assets
        DEPENDS ${ROOT}/firmware/components/codex_micro/assets/bg_${theme}.bin)
    target_sources(audio_preview PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/bg_${theme}.o)
endforeach()
target_sources(audio_preview PRIVATE ${ROOT}/firmware/main/codex_micro_app.cpp
    ${ROOT}/firmware/components/codex_micro/gfx.cpp)
target_include_directories(audio_preview PRIVATE ${ROOT}/firmware/components/codex_micro ${ROOT}/tests/stubs)
if(NOT WIN32)
    target_link_options(audio_preview PRIVATE -Wl,-z,noexecstack)
endif()

add_executable(settings_preview ../settings/settings_preview.cpp
    ${ROOT}/firmware/main/launcher_apps.cpp ${ROOT}/firmware/main/muse_keypad.c
    ${ROOT}/firmware/main/app_icons.c ${ROOT}/firmware/main/ui_font.c)
target_include_directories(settings_preview PRIVATE ${ROOT}/tests/settings/stubs ${ROOT}/tests/audio/stubs
    ${ROOT}/firmware/main ${ROOT}/firmware/components/codex_micro ${ROOT}/firmware/components/shared_audio
    ${IDF_ROOT}/components/json/cJSON)
target_link_libraries(settings_preview PRIVATE lvgl m)
target_compile_options(settings_preview PRIVATE -Wall -Wextra -Werror -UNDEBUG)
