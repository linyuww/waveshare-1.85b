#pragma once
#include "lvgl.h"
#include "ui_assets.h"

// Shared application frame: its visible text/content fits the circular panel.
struct LauncherAppShell { lv_obj_t *body; lv_obj_t *home; lv_obj_t *title; };
inline LauncherAppShell createLauncherAppShell(lv_obj_t *root, const char *name, bool battery_hint = false)
{
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(root, 8, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x101923), 0);
    lv_obj_set_style_text_font(root, &ui_font, 0);
    lv_obj_set_style_text_color(root, lv_color_hex(0xF1F5FA), 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_pad_top(root, 28, 0);
    lv_obj_set_style_pad_left(root, 28, 0);
    lv_obj_set_style_pad_right(root, 28, 0);
    lv_obj_set_style_pad_bottom(root, 60, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    auto *header = lv_obj_create(root);
    lv_obj_set_size(header, LV_PCT(100), 40);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    auto *home = lv_button_create(header);
    lv_obj_set_size(home, 70, 36);
    lv_obj_set_style_bg_color(home, lv_color_hex(0x203044), 0);
    lv_obj_set_style_radius(home, 12, 0);
    lv_obj_set_style_shadow_width(home, 0, 0);
    auto *home_text = lv_label_create(home);
    lv_obj_set_style_text_font(home_text, &ui_font, 0);
    lv_obj_set_style_text_color(home_text, lv_color_hex(0xF1F5FA), 0);
    lv_label_set_text(home_text, "返回");
    lv_obj_center(home_text);
    lv_obj_align(home, LV_ALIGN_LEFT_MID, 48, 0);
    auto *title = lv_label_create(header);
    lv_obj_set_style_text_font(title, &ui_font, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xF1F5FA), 0);
    lv_obj_set_width(title, 180);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(title, name);
    lv_obj_align(title, LV_ALIGN_RIGHT_MID, -48, 0);
    auto *body = lv_obj_create(root);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 10, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_radius(body, 0, 0);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x101923), 0);
    lv_obj_set_style_text_font(body, &ui_font, 0);
    lv_obj_set_style_text_color(body, lv_color_hex(0xF1F5FA), 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_style_pad_left(body, 20, 0);
    lv_obj_set_style_pad_right(body, 20, 0);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
    if (battery_hint) {
        // Nine complete 24px text lines, followed by a visible scroll hint.
        lv_obj_set_style_pad_bottom(root, 36, 0);
        auto *hint = lv_label_create(root);
        lv_obj_set_width(hint, LV_PCT(100));
        lv_obj_set_style_text_font(hint, &ui_font, 0);
        lv_obj_set_style_text_color(hint, lv_color_hex(0xAAB9CB), 0);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(hint, "上滑查看更多读数");
    }
    return {body, home, title};
}
