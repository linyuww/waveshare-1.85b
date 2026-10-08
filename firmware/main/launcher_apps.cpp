// Muse card/page/keypad layout adapted from muse_settings_ui.c (Apache-2.0).
// Copyright (c) Meta Platforms, Inc. and affiliates.
// Licensed under https://www.apache.org/licenses/LICENSE-2.0
#include "launcher_apps.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>
#include "bsp/esp-bsp.h"
#include "bluetooth_service.hpp"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ui_assets.h"
#include "ui_app_shell.hpp"
#include "audio_apps.hpp"
#include "muse_keypad.h"

namespace {
constexpr uint32_t BG = 0x101923;
constexpr uint32_t CARD = 0x1a1530;
constexpr uint32_t TEXT = 0xF1F5FA;
constexpr uint32_t MUTED = 0xAAB9CB;
enum Action { Home = 1, Scan, Manual, Forget, Connect, Cancel, Test, CloseKeyboard, ForgetConfirm, BluetoothToggle, Pair, PairConfirm, NavWifi, NavBluetooth, NavSound, NavAssistant, NavAbout, NavBattery, EditOta, EditWebsocket, EditToken };
const char *names[] = {"设置", "时钟", "网络测试", "设备信息"};
const lv_image_dsc_t *icons[] = {&icon_settings, &icon_clock, &icon_network, &icon_about};

lv_obj_t *label(lv_obj_t *parent, const char *text)
{
    auto *obj = lv_label_create(parent);
    lv_obj_set_width(obj, LV_PCT(100));
    lv_obj_set_style_text_font(obj, &ui_font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(TEXT), 0);
    lv_label_set_text(obj, text);
    return obj;
}

lv_obj_t *button(lv_obj_t *parent, const char *text, lv_event_cb_t callback, void *user, Action action)
{
    auto *obj = lv_button_create(parent);
    lv_obj_set_size(obj, LV_PCT(100), 44);
    lv_obj_set_style_bg_color(obj, lv_color_hex(CARD), 0);
    lv_obj_set_style_radius(obj, 12, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_user_data(obj, reinterpret_cast<void *>(static_cast<intptr_t>(action)));
    lv_obj_add_event_cb(obj, callback, LV_EVENT_CLICKED, user);
    auto *text_obj = label(obj, text);
    lv_obj_set_style_text_align(text_obj, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(text_obj);
    return obj;
}

lv_obj_t *switchRow(lv_obj_t *parent, const char *text, Action action, lv_event_cb_t callback, void *user)
{
    auto *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 54);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(CARD), 0);
    lv_obj_set_style_radius(row, 18, 0);
    lv_obj_set_style_pad_hor(row, 14, 0);
    auto *title = label(row, text);
    lv_obj_set_width(title, 170);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);
    auto *control = lv_switch_create(row);
    lv_obj_align(control, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(control, lv_color_hex(0xa77dff), static_cast<lv_style_selector_t>(LV_PART_INDICATOR) | static_cast<lv_style_selector_t>(LV_STATE_CHECKED));
    lv_obj_set_user_data(control, reinterpret_cast<void *>(static_cast<intptr_t>(action)));
    lv_obj_add_event_cb(control, callback, LV_EVENT_VALUE_CHANGED, user);
    return control;
}

lv_obj_t *actionRow(lv_obj_t *parent, const char *text, const char *value,
                    Action action, lv_event_cb_t callback, void *user)
{
    auto *row = button(parent, text, callback, user, action);
    lv_obj_set_height(row, 54);
    lv_obj_set_style_radius(row, 18, 0);
    lv_obj_set_style_pad_hor(row, 14, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x2e2552), LV_STATE_PRESSED);
    auto *caption = lv_obj_get_child(row, 0);
    lv_obj_set_width(caption, 155);
    lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(caption, LV_LABEL_LONG_DOT);
    lv_obj_align(caption, LV_ALIGN_LEFT_MID, 0, 0);
    auto *detail = label(row, value);
    lv_obj_set_width(detail, 90);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(detail, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(detail, lv_color_hex(0x8b84a8), 0);
    lv_obj_align(detail, LV_ALIGN_RIGHT_MID, 0, 0);
    return row;
}

lv_obj_t *infoRow(lv_obj_t *parent, const char *text)
{
    auto *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 44);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(CARD), 0);
    lv_obj_set_style_radius(row, 18, 0);
    lv_obj_set_style_pad_hor(row, 14, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    auto *caption = label(row, text);
    lv_obj_set_width(caption, 118);
    lv_obj_align(caption, LV_ALIGN_LEFT_MID, 0, 0);
    auto *value = label(row, "");
    lv_obj_set_width(value, 132);
    lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(value, lv_color_hex(0xa77dff), 0);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(value, LV_ALIGN_RIGHT_MID, 0, 0);
    return value;
}

void sliderRow(lv_obj_t *parent, const char *text, int low, int high, int current,
               lv_obj_t **value_out, lv_event_cb_t callback, void *user)
{
    auto *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 84);
    lv_obj_set_style_pad_hor(row, 8, 0);
    lv_obj_set_style_pad_ver(row, 6, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    auto *caption = label(row, text);
    lv_obj_set_width(caption, 130);
    *value_out = label(row, "");
    lv_obj_set_width(*value_out, 80);
    lv_obj_set_style_text_align(*value_out, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(*value_out, lv_color_hex(0xa77dff), 0);
    lv_obj_align(*value_out, LV_ALIGN_TOP_RIGHT, 0, 0);
    char value[16]; snprintf(value, sizeof(value), "%d%%", current);
    lv_label_set_text(*value_out, value);
    auto *slider = lv_slider_create(row);
    lv_obj_set_size(slider, LV_PCT(94), 12);
    lv_obj_align(slider, LV_ALIGN_TOP_MID, 0, 40);
    lv_slider_set_range(slider, low, high);
    lv_slider_set_value(slider, current, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x2a2345), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xa77dff), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(TEXT), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 6, LV_PART_KNOB);
    lv_obj_set_ext_click_area(slider, 16);
    lv_obj_add_event_cb(slider, callback, LV_EVENT_VALUE_CHANGED, user);
    lv_obj_add_event_cb(slider, callback, LV_EVENT_RELEASED, user);
}

void column(lv_obj_t *obj, int gap = 10)
{
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(obj, gap, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(BG), 0);
    lv_obj_set_style_text_font(obj, &ui_font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(TEXT), 0);
}
}

LauncherApp::LauncherApp(Kind kind)
    : App(names[static_cast<int>(kind)], icons[static_cast<int>(kind)], true, true, false), kind_(kind)
{
}

bool LauncherApp::run()
{
    root_ = lv_screen_active();
    if (kind_ == Kind::Settings || kind_ == Kind::About) {
        lv_obj_set_style_bg_color(root_, lv_color_hex(0x0c0918), 0);
        lv_obj_set_style_pad_all(root_, 0, 0);
        lv_obj_set_style_border_width(root_, 0, 0);
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
        showSettingsPage(kind_ == Kind::About ? SettingsPage::About : SettingsPage::Home);
        lv_timer_create(onTimer, 400, this);
        return true;
    }
    const auto shell = createLauncherAppShell(root_, names[static_cast<int>(kind_)], kind_ == Kind::About);
    lv_obj_set_user_data(shell.home, reinterpret_cast<void *>(static_cast<intptr_t>(Home)));
    lv_obj_add_event_cb(shell.home, onAction, LV_EVENT_CLICKED, this);
    auto *body = shell.body;
    switch (kind_) {
    case Kind::Settings: buildSettings(body); break;
    case Kind::Clock: buildClock(body); break;
    case Kind::Network: buildNetwork(body); break;
    case Kind::About: buildAbout(body); break;
    }
    // Brookesia records this timer during run() and deletes it with the app screen.
    lv_timer_create(onTimer, 400, this);
    refresh();
    return true;
}

bool LauncherApp::back()
{
    if (form_) { hideConnectionForm(); return true; }
    if (kind_ == Kind::Settings && settings_page_kind_ != SettingsPage::Home) { showSettingsPage(SettingsPage::Home); return true; }
    return notifyCoreClosed();
}

bool LauncherApp::close()
{
    confirmation_ = nullptr;
    root_ = message_ = network_list_ = clock_ = date_ = result_ = battery_label_ = nullptr;
    scan_button_ = test_button_ = brightness_label_ = nullptr;
    bluetooth_label_ = bluetooth_address_ = bluetooth_switch_ = pair_button_ = nullptr;
    form_ = ssid_input_ = password_input_ = keyboard_ = nullptr;
    settings_page_ = settings_title_ = volume_label_ = nullptr;
    for (auto &value : home_values_) value = nullptr;
    memset(join_ssid_, 0, sizeof(join_ssid_));
    scan_revision_ = UINT32_MAX;
    listed_ap_count_ = 0;
    return true;
}

// Adapted from Muse muse_settings_ui.c (Apache-2.0): cards, lazy pages and swipe-back.
void LauncherApp::showSettingsPage(SettingsPage page)
{
    hideConnectionForm();
    confirmation_ = nullptr;
    for (auto &value : battery_values_) value = nullptr;
    for (auto &value : device_values_) value = nullptr;
    for (auto &value : assistant_values_) value = nullptr;
    if (settings_page_) {
        lv_obj_add_flag(settings_page_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_delete_async(settings_page_);
    }
    message_ = network_list_ = scan_button_ = brightness_label_ = nullptr;
    bluetooth_label_ = bluetooth_address_ = bluetooth_switch_ = pair_button_ = nullptr;
    battery_label_ = result_ = volume_label_ = nullptr;
    for (auto &value : home_values_) value = nullptr;
    scan_revision_ = UINT32_MAX;
    settings_page_kind_ = page;
    settings_page_ = lv_obj_create(root_);
    lv_obj_remove_style_all(settings_page_);
    lv_obj_set_size(settings_page_, LV_PCT(100), LV_PCT(100));
    lv_obj_remove_flag(settings_page_, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE));
    lv_obj_add_event_cb(settings_page_, onGesture, LV_EVENT_GESTURE, this);
    auto *back = button(settings_page_, "", onAction, this, Home);
    lv_obj_remove_style_all(back);
    lv_obj_set_size(back, 44, 40);
    lv_obj_align(back, LV_ALIGN_TOP_MID, -90, 32);
    auto *arrow = lv_obj_get_child(back, 0);
    lv_obj_set_style_text_font(arrow, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(arrow, lv_color_hex(0xa77dff), 0);
    lv_label_set_text(arrow, LV_SYMBOL_LEFT);
    const char *titles[] = {"设置", "Wi-Fi", "蓝牙", "声音与显示", "小智助手", "设备信息", "电池"};
    settings_title_ = label(settings_page_, titles[static_cast<int>(page)]);
    lv_obj_set_width(settings_title_, 160);
    lv_obj_set_style_text_align(settings_title_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(settings_title_, lv_color_hex(0xa77dff), 0);
    lv_obj_align(settings_title_, LV_ALIGN_TOP_MID, 12, 42);
    auto *body = lv_obj_create(settings_page_);
    lv_obj_remove_style_all(body);
    // The complete viewport stays inside the 360 px round panel.
    lv_obj_set_size(body, 280, 204);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 10, 0);
    lv_obj_set_style_pad_bottom(body, 12, 0);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
    buildSettings(body);
    refresh();
}

void LauncherApp::onGesture(lv_event_t *event)
{
    auto *pressed = lv_indev_get_active_obj();
    if (pressed && lv_obj_check_type(pressed, &lv_slider_class)) return;
    auto *input = lv_indev_active();
    if (input && lv_indev_get_gesture_dir(input) == LV_DIR_RIGHT) {
        lv_indev_wait_release(input);
        static_cast<LauncherApp *>(lv_event_get_user_data(event))->back();
    }
}

void LauncherApp::onVolume(lv_event_t *event)
{
    auto *self = static_cast<LauncherApp *>(lv_event_get_user_data(event));
    const int value = lv_slider_get_value(lv_event_get_target_obj(event));
    char text[64];
    snprintf(text, sizeof(text), "%d%%", value);
    lv_label_set_text(self->volume_label_, text);
    if (lv_event_get_code(event) == LV_EVENT_RELEASED && !AssistantService::instance().setVolume(value))
        lv_label_set_text(self->volume_label_, "服务忙，请再次调整音量");
}

void LauncherApp::buildSettings(lv_obj_t *body)
{
    switch (settings_page_kind_) {
    case SettingsPage::Home: {
        const char *titles[] = {"Wi-Fi", "蓝牙", "声音与显示", "小智助手", "设备信息", "电池"};
        const char *icons[] = {LV_SYMBOL_WIFI, LV_SYMBOL_BLUETOOTH, LV_SYMBOL_VOLUME_MAX, LV_SYMBOL_AUDIO, LV_SYMBOL_SETTINGS, LV_SYMBOL_BATTERY_FULL};
        for (int i = 0; i < 6; ++i) {
            auto *card = button(body, "", onAction, this, static_cast<Action>(NavWifi + i));
            lv_obj_set_height(card, 58);
            lv_obj_set_style_radius(card, 18, 0);
            lv_obj_set_style_bg_color(card, lv_color_hex(0x2e2552), LV_STATE_PRESSED);
            lv_obj_set_style_pad_hor(card, 14, 0);
            auto *symbol = lv_obj_get_child(card, 0);
            lv_obj_set_width(symbol, 24);
            lv_obj_set_style_text_font(symbol, &lv_font_montserrat_18, 0);
            lv_obj_set_style_text_color(symbol, lv_color_hex(0xa77dff), 0);
            lv_label_set_text(symbol, icons[i]);
            lv_obj_align(symbol, LV_ALIGN_LEFT_MID, 0, 0);
            auto *caption = label(card, titles[i]);
            lv_obj_set_width(caption, 130);
            lv_obj_align(caption, LV_ALIGN_LEFT_MID, 34, 0);
            home_values_[i] = label(card, "");
            lv_obj_set_width(home_values_[i], 74);
            lv_label_set_long_mode(home_values_[i], LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_align(home_values_[i], LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_style_text_color(home_values_[i], lv_color_hex(0x8b84a8), 0);
            lv_obj_align(home_values_[i], LV_ALIGN_RIGHT_MID, 0, 0);
        }
        break;
    }
    case SettingsPage::Wifi:
        message_ = label(body, "");
        scan_button_ = button(body, "扫描 Wi-Fi", onAction, this, Scan);
        network_list_ = lv_obj_create(body);
        lv_obj_set_size(network_list_, LV_PCT(100), LV_SIZE_CONTENT);
        column(network_list_, 8);
        lv_obj_set_style_bg_opa(network_list_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(network_list_, 0, 0);
        lv_obj_remove_flag(network_list_, LV_OBJ_FLAG_SCROLLABLE);
        button(body, "其他网络", onAction, this, Manual);
        button(body, "忘记当前网络", onAction, this, Forget);
        break;
    case SettingsPage::Bluetooth: {
        bluetooth_switch_ = switchRow(body, "蓝牙", BluetoothToggle, onAction, this);
        bluetooth_label_ = label(body, "");
        lv_label_set_text(infoRow(body, "名称"), "Codex Micro");
        bluetooth_address_ = infoRow(body, "地址");
        lv_obj_set_width(lv_obj_get_child(lv_obj_get_parent(bluetooth_address_), 0), 40);
        lv_obj_set_width(bluetooth_address_, 210);
        pair_button_ = button(body, "重新配对", onAction, this, Pair);
        break;
    }
    case SettingsPage::Sound:
        sliderRow(body, "音量", 0, 100, AssistantService::instance().config().volume, &volume_label_, onVolume, this);
        sliderRow(body, "亮度", 10, 100, SystemService::instance().snapshot().brightness, &brightness_label_, onBrightness, this);
        break;
    case SettingsPage::Assistant: {
        message_ = label(body, "");
        const auto config = AssistantService::instance().config();
        assistant_values_[0] = lv_obj_get_child(actionRow(body, "激活服务地址", config.ota_url, EditOta, onAction, this), 1);
        assistant_values_[1] = lv_obj_get_child(actionRow(body, "WebSocket", config.websocket_url[0] ? config.websocket_url : "默认", EditWebsocket, onAction, this), 1);
        assistant_values_[2] = lv_obj_get_child(actionRow(body, "令牌", config.token[0] ? "已设置" : "--", EditToken, onAction, this), 1);
        break;
    }
    case SettingsPage::About: buildAbout(body); break;
    case SettingsPage::Battery: {
        battery_label_ = label(body, "");
        const char *names[] = {"估算电量", "电量计 SOC", "电压", "电流", "温度", "剩余容量", "满充容量", "健康度", "循环次数", "供电"};
        for (int i = 0; i < 10; ++i) battery_values_[i] = infoRow(body, names[i]);
        break;
    }
    }
}

void LauncherApp::buildClock(lv_obj_t *body)
{
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    clock_ = label(body, "--:--:--");
    lv_obj_set_style_text_font(clock_, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_align(clock_, LV_TEXT_ALIGN_CENTER, 0);
    date_ = label(body, "");
    lv_obj_set_style_text_align(date_, LV_TEXT_ALIGN_CENTER, 0);
    message_ = label(body, "");
    lv_obj_set_style_text_align(message_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(message_, lv_color_hex(MUTED), 0);
}

void LauncherApp::buildNetwork(lv_obj_t *body)
{
    message_ = label(body, "");
    result_ = label(body, "");
    test_button_ = button(body, "测试 HTTPS 访问", onAction, this, Test);
    auto *hint = label(body, "测试 DNS、校时及 HTTPS 证书。\n首次联网校时后自动检查一次。");
    lv_obj_set_style_text_color(hint, lv_color_hex(MUTED), 0);
}

void LauncherApp::buildAbout(lv_obj_t *body)
{
    const auto *app = esp_app_get_description();
    lv_label_set_text(infoRow(body, "设备"), "ESP32-S3");
    lv_label_set_text(infoRow(body, "固件"), app->version);
    lv_label_set_text(infoRow(body, "ESP-IDF"), app->idf_ver);
    auto *mac = infoRow(body, "MAC");
    lv_obj_set_width(lv_obj_get_child(lv_obj_get_parent(mac), 0), 40);
    lv_obj_set_width(mac, 210);
    lv_label_set_text(mac, SystemService::instance().snapshot().mac);
    lv_label_set_text(infoRow(body, "屏幕"), "360 × 360");
    lv_label_set_text(infoRow(body, "Flash"), "16 MB");
    const char *names[] = {"内部空闲", "内部最大块", "PSRAM 空闲"};
    for (int i = 0; i < 3; ++i) device_values_[i] = infoRow(body, names[i]);
}

void LauncherApp::onTimer(lv_timer_t *timer)
{
    static_cast<LauncherApp *>(lv_timer_get_user_data(timer))->refresh();
}

void LauncherApp::refresh()
{
    if (!root_) return;
    const auto status = SystemService::instance().snapshot();
    char text[1536];
    if (confirmation_ && lv_tick_elaps(confirmation_at_) >= 4000) {
        if (confirmation_action_ == Forget) lv_label_set_text(lv_obj_get_child(confirmation_, 0), "忘记当前网络");
        else if (confirmation_action_ == Pair) lv_label_set_text(lv_obj_get_child(confirmation_, 0), "重新配对");
        lv_obj_set_user_data(confirmation_, reinterpret_cast<void *>(static_cast<intptr_t>(confirmation_action_)));
        confirmation_ = nullptr;
    }
    if (kind_ == Kind::Clock) {
        if (SystemService::timeValid()) {
            const time_t now = time(nullptr);
            tm local = {};
            localtime_r(&now, &local);
            strftime(text, sizeof(text), "%H:%M:%S", &local);
            lv_label_set_text(clock_, text);
            snprintf(text, sizeof(text), "%04d 年 %02d 月 %02d 日", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
            lv_label_set_text(date_, text);
            lv_label_set_text(message_, "北京时间");
        } else {
            lv_label_set_text(clock_, "--:--:--");
            lv_label_set_text(date_, "等待联网校时");
            lv_label_set_text(message_, status.state == SystemService::NetworkState::Connected ? "正在同步时间" : "请先在设置中连接 Wi-Fi");
        }
    } else if (kind_ == Kind::Settings && settings_page_kind_ == SettingsPage::Home) {
        const auto bluetooth = BluetoothService::instance().snapshot();
        lv_label_set_text(home_values_[0], SystemService::stateText(status.state));
        lv_label_set_text(home_values_[1], bluetooth.enabled ? "已开启" : "已关闭");
        snprintf(text, sizeof(text), "%d%%", AssistantService::instance().config().volume);
        lv_label_set_text(home_values_[2], text);
        lv_label_set_text(home_values_[3], AssistantService::instance().snapshot().connected ? "已连接" : "未连接");
        lv_label_set_text(home_values_[4], "查看");
        snprintf(text, sizeof(text), "%d%%", bluetooth.battery_percent);
        lv_label_set_text(home_values_[5], bluetooth.battery_percent >= 0 ? text : "--");
    } else if (kind_ == Kind::Settings && settings_page_kind_ == SettingsPage::Assistant) {
        lv_label_set_text(message_, AssistantService::instance().snapshot().connected ? "已连接" : "未连接");
        const auto config = AssistantService::instance().config();
        lv_label_set_text(assistant_values_[0], config.ota_url);
        lv_label_set_text(assistant_values_[1], config.websocket_url[0] ? config.websocket_url : "默认");
        lv_label_set_text(assistant_values_[2], config.token[0] ? "已设置" : "--");
    } else if (kind_ == Kind::Settings && settings_page_kind_ == SettingsPage::Battery) {
        const auto sample = BluetoothService::instance().snapshot().battery;
        lv_label_set_text(battery_label_, !sample.valid ? "等待采样" : sample.stale ? "上次采样" : "实时采样");
        for (auto *value : battery_values_) lv_label_set_text(value, "--");
        if (sample.valid) {
            snprintf(text, sizeof(text), "%d%%", sample.percent); lv_label_set_text(battery_values_[0], text);
            if (sample.gaugePercent >= 0) { snprintf(text, sizeof(text), "%d%%", sample.gaugePercent); lv_label_set_text(battery_values_[1], text); }
            snprintf(text, sizeof(text), "%u mV", sample.voltageMv); lv_label_set_text(battery_values_[2], text);
            snprintf(text, sizeof(text), "%d mA", sample.currentMa); lv_label_set_text(battery_values_[3], text);
            snprintf(text, sizeof(text), "%.1f °C", sample.temperatureDeciC / 10.0); lv_label_set_text(battery_values_[4], text);
            snprintf(text, sizeof(text), "%u mAh", sample.remainingCapacityMah); lv_label_set_text(battery_values_[5], text);
            snprintf(text, sizeof(text), "%u mAh", sample.fullChargeCapacityMah); lv_label_set_text(battery_values_[6], text);
            if (sample.healthPercent >= 0) { snprintf(text, sizeof(text), "%d%%", sample.healthPercent); lv_label_set_text(battery_values_[7], text); }
            snprintf(text, sizeof(text), "%u", sample.cycleCount); lv_label_set_text(battery_values_[8], text);
            lv_label_set_text(battery_values_[9], sample.charging ? "充电中" : sample.externalPower ? "外部供电" : "电池");
        }
    } else if (kind_ == Kind::Settings && settings_page_kind_ == SettingsPage::Wifi) {
        snprintf(text, sizeof(text), "%s%s%s", SystemService::stateText(status.state), status.ssid[0] ? " · " : "", status.ssid);
        lv_label_set_text(message_, text);
        lv_label_set_text(lv_obj_get_child(scan_button_, 0), status.scanning ? "正在扫描…" : "扫描 Wi-Fi");
        if (!status.ready || status.scanning || status.connection_pending || status.state == SystemService::NetworkState::Connecting) lv_obj_add_state(scan_button_, LV_STATE_DISABLED);
        else lv_obj_remove_state(scan_button_, LV_STATE_DISABLED);
        if (scan_revision_ != status.scan_revision) updateScanList(status);
    } else if (kind_ == Kind::Settings && settings_page_kind_ == SettingsPage::Bluetooth) {
        const auto bluetooth = BluetoothService::instance().snapshot();
        snprintf(text, sizeof(text), "%s", !bluetooth.ready ? "正在启动" : !bluetooth.enabled ? "已关闭" : bluetooth.connected ? "已连接" : bluetooth.advertising ? "等待连接" : "准备广播");
        lv_label_set_text(bluetooth_label_, text);
        lv_label_set_text(bluetooth_address_, bluetooth.address[0] ? bluetooth.address : "--");
        if (bluetooth.enabled) lv_obj_add_state(bluetooth_switch_, LV_STATE_CHECKED);
        else lv_obj_remove_state(bluetooth_switch_, LV_STATE_CHECKED);
        for (auto *control : {bluetooth_switch_, pair_button_}) {
            if (!bluetooth.ready || bluetooth.busy) lv_obj_add_state(control, LV_STATE_DISABLED);
            else lv_obj_remove_state(control, LV_STATE_DISABLED);
        }
    } else if (kind_ == Kind::Network) {
        snprintf(text, sizeof(text), "网络：%s\nSSID：%s\nIP：%s\n信号：%d dBm", SystemService::stateText(status.state), status.ssid[0] ? status.ssid : "--", status.ip[0] ? status.ip : "--", status.rssi);
        lv_label_set_text(message_, text);
        lv_label_set_text(result_, status.test_result);
        lv_label_set_text(lv_obj_get_child(test_button_, 0), status.testing ? "正在测试…" : "测试 HTTPS 访问");
        if (status.testing || status.state != SystemService::NetworkState::Connected) lv_obj_add_state(test_button_, LV_STATE_DISABLED);
        else lv_obj_remove_state(test_button_, LV_STATE_DISABLED);
    } else if (kind_ == Kind::About || (kind_ == Kind::Settings && settings_page_kind_ == SettingsPage::About)) {
        const size_t sizes[] = {
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
            heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM)
        };
        for (int i = 0; i < 3; ++i) {
            snprintf(text, sizeof(text), "%u KB", static_cast<unsigned>(sizes[i] / 1024));
            lv_label_set_text(device_values_[i], text);
        }
    }
}

void LauncherApp::updateScanList(const SystemService::Snapshot &status)
{
    scan_revision_ = status.scan_revision;
    listed_ap_count_ = status.ap_count;
    memcpy(listed_aps_, status.aps, sizeof(listed_aps_));
    lv_obj_clean(network_list_);
    for (int i = 0; i < status.ap_count; ++i) {
        char signal[24];
        snprintf(signal, sizeof(signal), "%d dBm", status.aps[i].rssi);
        auto *obj = actionRow(network_list_, status.aps[i].ssid, signal, Scan, onNetworkSelected, this);
        lv_obj_set_user_data(obj, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        lv_label_set_long_mode(lv_obj_get_child(obj, 0), LV_LABEL_LONG_DOT);
    }
}

void LauncherApp::onNetworkSelected(lv_event_t *event)
{
    auto *self = static_cast<LauncherApp *>(lv_event_get_user_data(event));
    const auto index = reinterpret_cast<intptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(event)));
    // Select the result actually displayed, even if a new scan completed between frames.
    if (index >= 0 && index < self->listed_ap_count_) self->showConnectionForm(self->listed_aps_[index].ssid, self->listed_aps_[index].secured);
}

void LauncherApp::showConnectionForm(const char *ssid, bool secured)
{
    secured_ = secured;
    allow_empty_password_ = !ssid[0];
    snprintf(join_ssid_, sizeof(join_ssid_), "%s", ssid);
    openText(ssid[0] ? TextField::Password : TextField::Ssid, "");
    if (ssid[0] && !secured) submitConnection();
}

void LauncherApp::openText(TextField field, const char *text)
{
    hideConnectionForm();
    text_field_ = field;
    lv_obj_add_flag(settings_page_, LV_OBJ_FLAG_HIDDEN);
    form_ = lv_obj_create(root_);
    lv_obj_remove_style_all(form_);
    lv_obj_set_size(form_, LV_PCT(100), LV_PCT(100));
    lv_obj_remove_flag(form_, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE));
    lv_obj_add_event_cb(form_, onGesture, LV_EVENT_GESTURE, this);
    const char *titles[] = {"网络名称 SSID", "Wi-Fi 密码", "激活服务地址", "WebSocket", "令牌"};
    auto *title = label(form_, titles[static_cast<int>(field)]);
    lv_obj_set_width(title, 210);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);
    auto *cancel = button(form_, "", onAction, this, Cancel);
    lv_obj_set_size(cancel, 38, 36);
    lv_obj_align(cancel, LV_ALIGN_TOP_MID, -108, 51);
    auto *arrow = lv_obj_get_child(cancel, 0);
    lv_obj_set_style_text_font(arrow, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(arrow, lv_color_hex(0xa77dff), 0);
    lv_label_set_text(arrow, LV_SYMBOL_LEFT);
    password_input_ = lv_textarea_create(form_);
    lv_obj_set_size(password_input_, 186, 36);
    lv_obj_align(password_input_, LV_ALIGN_TOP_MID, 10, 51);
    lv_obj_set_style_text_font(password_input_, &ui_font, 0);
    lv_obj_set_style_pad_all(password_input_, 4, 0);
    lv_obj_set_style_bg_color(password_input_, lv_color_hex(CARD), 0);
    lv_obj_set_style_text_color(password_input_, lv_color_hex(TEXT), 0);
    lv_obj_set_style_border_color(password_input_, lv_color_hex(0xa77dff), 0);
    lv_obj_set_style_border_width(password_input_, 2, 0);
    lv_obj_set_style_radius(password_input_, 10, 0);
    lv_textarea_set_one_line(password_input_, true);
    const unsigned limits[] = {32, 64, 255, 255, 511};
    lv_textarea_set_max_length(password_input_, limits[static_cast<int>(field)]);
    const bool secret = field == TextField::Password || field == TextField::Token;
    lv_textarea_set_password_mode(password_input_, secret);
    lv_textarea_set_text(password_input_, text);
    keyboard_ = muse_keypad_create(form_, password_input_, true);
    lv_obj_set_size(keyboard_, 320, 230);
    lv_obj_align(keyboard_, LV_ALIGN_TOP_MID, 0, 94);
    muse_keypad_reset(keyboard_, secret);
    lv_obj_add_event_cb(keyboard_, onKeyboard, LV_EVENT_READY, this);
}

void LauncherApp::hideConnectionForm()
{
    if (password_input_) lv_textarea_set_text(password_input_, "");
    if (form_) {
        lv_obj_add_flag(form_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_delete_async(form_);
    }
    form_ = ssid_input_ = password_input_ = keyboard_ = nullptr;
    if (settings_page_) lv_obj_remove_flag(settings_page_, LV_OBJ_FLAG_HIDDEN);
}

void LauncherApp::submitConnection()
{
    const char *text = lv_textarea_get_text(password_input_);
    if (text_field_ == TextField::Ssid) {
        if (!text[0] || strlen(text) > 32) {
            lv_label_set_text(lv_obj_get_child(form_, 0), "检查名称长度");
            return;
        }
        snprintf(join_ssid_, sizeof(join_ssid_), "%s", text);
        openText(TextField::Password, "");
        return;
    }
    if (text_field_ != TextField::Password) {
        auto config = AssistantService::instance().config();
        if (text_field_ == TextField::Ota) snprintf(config.ota_url, sizeof(config.ota_url), "%s", text);
        if (text_field_ == TextField::Websocket) snprintf(config.websocket_url, sizeof(config.websocket_url), "%s", text);
        if (text_field_ == TextField::Token) snprintf(config.token, sizeof(config.token), "%s", text);
        if (!AssistantService::instance().saveConfig(config)) {
            lv_label_set_text(lv_obj_get_child(form_, 0), "地址无效或服务忙");
            return;
        }
        hideConnectionForm();
        lv_label_set_text(message_, "已提交保存");
        return;
    }
    const char *password = secured_ ? text : "";
    const size_t length = strlen(password);
    bool hex = length == 64;
    for (size_t i = 0; hex && i < length; ++i)
        if (!((password[i] >= '0' && password[i] <= '9') || (password[i] >= 'a' && password[i] <= 'f') || (password[i] >= 'A' && password[i] <= 'F'))) hex = false;
    if ((secured_ && !allow_empty_password_ && length == 0) || (length > 0 && length < 8) || (length == 64 && !hex)) {
        lv_label_set_text(lv_obj_get_child(form_, 0), "检查密码长度");
        return;
    }
    if (!SystemService::instance().connect(join_ssid_, password)) {
        lv_label_set_text(lv_obj_get_child(form_, 0), "服务忙，请稍后重试");
        return;
    }
    hideConnectionForm();
    memset(join_ssid_, 0, sizeof(join_ssid_));
    refresh();
}

void LauncherApp::onKeyboard(lv_event_t *event)
{
    static_cast<LauncherApp *>(lv_event_get_user_data(event))->submitConnection();
}

void LauncherApp::onBrightness(lv_event_t *event)
{
    auto *self = static_cast<LauncherApp *>(lv_event_get_user_data(event));
    const int value = lv_slider_get_value(lv_event_get_target_obj(event));
    if (lv_event_get_code(event) == LV_EVENT_VALUE_CHANGED) {
        const esp_err_t err = bsp_display_brightness_set(value);
        char text[64];
        snprintf(text, sizeof(text), err == ESP_OK ? "%d%%" : "调整失败：%d", value);
        lv_label_set_text(self->brightness_label_, text);
    } else if (!SystemService::instance().saveBrightness(value)) {
        lv_label_set_text(self->brightness_label_, "保存忙，请再次调整亮度");
    }
}

void LauncherApp::onAction(lv_event_t *event)
{
    auto *self = static_cast<LauncherApp *>(lv_event_get_user_data(event));
    auto *target = lv_event_get_target_obj(event);
    const auto action = static_cast<Action>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
    auto &service = SystemService::instance();
    switch (action) {
    case Home: self->back(); return;
    case NavWifi: self->showSettingsPage(SettingsPage::Wifi); return;
    case NavBluetooth: self->showSettingsPage(SettingsPage::Bluetooth); return;
    case NavSound: self->showSettingsPage(SettingsPage::Sound); return;
    case NavAssistant: self->showSettingsPage(SettingsPage::Assistant); return;
    case NavBattery: self->showSettingsPage(SettingsPage::Battery); return;
    case NavAbout: self->showSettingsPage(SettingsPage::About); return;
    case EditOta: self->openText(TextField::Ota, AssistantService::instance().config().ota_url); return;
    case EditWebsocket: self->openText(TextField::Websocket, AssistantService::instance().config().websocket_url); return;
    case EditToken: self->openText(TextField::Token, AssistantService::instance().config().token); return;
    case Scan:
        if (!service.scan()) lv_label_set_text(self->message_, "正在连接或扫描，请稍后重试");
        break;
    case Manual: self->showConnectionForm("", true); break;
    case Forget:
        self->confirmation_ = target; self->confirmation_at_ = lv_tick_get(); self->confirmation_action_ = Forget;
        lv_obj_set_user_data(target, reinterpret_cast<void *>(ForgetConfirm));
        lv_label_set_text(lv_obj_get_child(target, 0), "再次点击忘记");
        break;
    case ForgetConfirm:
        self->confirmation_ = nullptr;
        if (!service.forget()) lv_label_set_text(self->message_, "服务忙，请稍后重试");
        lv_obj_set_user_data(target, reinterpret_cast<void *>(Forget));
        lv_label_set_text(lv_obj_get_child(target, 0), "忘记当前网络");
        break;
    case Connect: self->submitConnection(); break;
    case Cancel: self->hideConnectionForm(); break;
    case Test:
        if (!service.testInternet()) lv_label_set_text(self->result_, "请先联网，或等待当前测试完成");
        break;
    case BluetoothToggle:
        if (!BluetoothService::instance().setEnabled(lv_obj_has_state(target, LV_STATE_CHECKED))) lv_label_set_text(self->bluetooth_label_, "蓝牙服务忙，请稍后重试");
        break;
    case Pair:
        self->confirmation_ = target; self->confirmation_at_ = lv_tick_get(); self->confirmation_action_ = Pair;
        lv_obj_set_user_data(target, reinterpret_cast<void *>(PairConfirm));
        lv_label_set_text(lv_obj_get_child(target, 0), "再次点击清除配对");
        break;
    case PairConfirm:
        self->confirmation_ = nullptr;
        if (!BluetoothService::instance().pairAgain()) lv_label_set_text(self->bluetooth_label_, "蓝牙服务忙，请稍后重试");
        lv_obj_set_user_data(target, reinterpret_cast<void *>(Pair));
        lv_label_set_text(lv_obj_get_child(target, 0), "重新配对");
        break;
    default: break;
    }
}
