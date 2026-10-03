#pragma once
// Shared by firmware styles and the LVGL desktop preview. Pixel coordinates on 360x360.
namespace launcher_layout {
constexpr int screen = 360;
constexpr int top = 56; // Original fixed status bar + 20px downwards.
constexpr int height = 304;
constexpr int table_width = 288;
constexpr int table_height = 238;
constexpr int tile = 112;
constexpr int icon = 80;
constexpr int icon_pressed = 72;
constexpr int label_gap = 4;
constexpr int indicator_height = 20;
constexpr int indicator_bottom = 32;
constexpr int indicator_gap = 22;
constexpr int dot = 8;
constexpr int active_dot_width = 28;
}
