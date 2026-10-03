/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "systems/phone/widgets/app_launcher/esp_brookesia_app_launcher.hpp"
#include "ui_assets.h"
#include "ui_layout.hpp"

namespace esp_brookesia::systems::phone {

constexpr AppLauncherIcon::Data STYLESHEET_360_360_DARK_APP_LAUNCHER_ICON_DATA = {
    .main = {
        .size = gui::StyleSize::SQUARE(launcher_layout::tile),
        .layout_row_pad = launcher_layout::label_gap,
    },
    .image = {
        .default_size = gui::StyleSize::SQUARE(launcher_layout::icon),
        .press_size = gui::StyleSize::SQUARE(launcher_layout::icon_pressed),
    },
    .label = {
        .text_font = gui::StyleFont::CUSTOM_SIZE(18, &ui_font),
        .text_color = gui::StyleColor::COLOR(0xFFFFFF),
    },
};

constexpr AppLauncherData STYLESHEET_360_360_DARK_APP_LAUNCHER_DATA = {
    .main = {
        .y_start = launcher_layout::top,
        .size = gui::StyleSize::RECT(launcher_layout::screen, launcher_layout::height),
    },
    .table = {
        .default_num = 1,
        .size = gui::StyleSize::RECT(launcher_layout::table_width, launcher_layout::table_height),
    },
    .indicator = {
        .main_size = gui::StyleSize::RECT(launcher_layout::screen, launcher_layout::indicator_height),
        .main_layout_column_pad = launcher_layout::indicator_gap,
        .main_layout_bottom_offset = launcher_layout::indicator_bottom,
        .spot_inactive_size = gui::StyleSize::SQUARE(launcher_layout::dot),
        .spot_active_size = gui::StyleSize::RECT(launcher_layout::active_dot_width, launcher_layout::dot),
        .spot_inactive_background_color = gui::StyleColor::COLOR(0x5F7C96),
        .spot_active_background_color = gui::StyleColor::COLOR(0xFFFFFF),
    },
    .icon = STYLESHEET_360_360_DARK_APP_LAUNCHER_ICON_DATA,
    .flags = {
        .enable_table_scroll_anim = 0,
    },
};

} // namespace esp_brookesia::systems::phone
