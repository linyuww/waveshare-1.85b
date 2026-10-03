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
#include "battery_info.hpp"
#include "audio_apps.hpp"

namespace {
constexpr uint32_t BG = 0x101923;
constexpr uint32_t CARD = 0x203044;
constexpr uint32_t TEXT = 0xF1F5FA;
constexpr uint32_t MUTED = 0xAAB9CB;
enum Action { Home = 1, Scan, Manual, Forget, Connect, Cancel, Test, CloseKeyboard, ForgetConfirm, BluetoothToggle, Pair, PairConfirm };
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
    return notifyCoreClosed();
}

bool LauncherApp::close()
{
    root_ = message_ = network_list_ = clock_ = date_ = result_ = battery_label_ = nullptr;
    scan_button_ = test_button_ = brightness_label_ = nullptr;
    bluetooth_label_ = bluetooth_switch_ = pair_button_ = nullptr;
    form_ = ssid_input_ = password_input_ = keyboard_ = nullptr;
    scan_revision_ = UINT32_MAX;
    listed_ap_count_ = 0;
    return true;
}

void LauncherApp::buildSettings(lv_obj_t *body)
{
    label(body, "Wi-Fi");
    message_ = label(body, "");
    scan_button_ = button(body, "扫描 Wi-Fi", onAction, this, Scan);
    network_list_ = lv_obj_create(body);
    lv_obj_set_size(network_list_, LV_PCT(100), LV_SIZE_CONTENT);
    column(network_list_, 6);
    lv_obj_set_style_pad_all(network_list_, 0, 0);
    lv_obj_remove_flag(network_list_, LV_OBJ_FLAG_SCROLLABLE);
    button(body, "手动输入网络", onAction, this, Manual);
    button(body, "忘记当前网络", onAction, this, Forget);
    label(body, "蓝牙 BLE");
    bluetooth_label_ = label(body, "正在启动蓝牙服务");
    auto *bluetooth_row = lv_obj_create(body);
    lv_obj_set_size(bluetooth_row, LV_PCT(100), 42);
    lv_obj_set_style_pad_all(bluetooth_row, 0, 0);
    lv_obj_set_style_border_width(bluetooth_row, 0, 0);
    lv_obj_set_style_bg_opa(bluetooth_row, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(bluetooth_row, LV_OBJ_FLAG_SCROLLABLE);
    auto *bluetooth_title = label(bluetooth_row, "启用蓝牙");
    lv_obj_set_width(bluetooth_title, 180);
    lv_obj_align(bluetooth_title, LV_ALIGN_LEFT_MID, 0, 0);
    bluetooth_switch_ = lv_switch_create(bluetooth_row);
    lv_obj_align(bluetooth_switch_, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_user_data(bluetooth_switch_, reinterpret_cast<void *>(BluetoothToggle));
    lv_obj_add_event_cb(bluetooth_switch_, onAction, LV_EVENT_VALUE_CHANGED, this);
    pair_button_ = button(body, "重新配对", onAction, this, Pair);
    auto *bluetooth_hint = label(body, "在电脑蓝牙设置中添加 Codex Micro。\n重新配对会清除旧配对信息。");
    lv_obj_set_style_text_color(bluetooth_hint, lv_color_hex(MUTED), 0);
    label(body, "显示");
    const auto status = SystemService::instance().snapshot();
    char text[40];
    snprintf(text, sizeof(text), "屏幕亮度：%d%%", status.brightness);
    brightness_label_ = label(body, text);
    auto *slider = lv_slider_create(body);
    lv_obj_set_size(slider, LV_PCT(90), 18);
    lv_slider_set_range(slider, 10, 100);
    lv_slider_set_value(slider, status.brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, onBrightness, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(slider, onBrightness, LV_EVENT_RELEASED, this);
    auto *hint = label(body, "配网成功后，所有应用共享网络。\n仅支持 2.4 GHz Wi-Fi。");
    lv_obj_set_style_text_color(hint, lv_color_hex(MUTED), 0);
    buildSharedAudioSettings(body);
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
    battery_label_ = label(body, "正在读取电池…");
    char text[320];
    const auto status = SystemService::instance().snapshot();
    snprintf(text, sizeof(text), "ESP32-S3-Touch-LCD-1.85B\n\n固件版本：%s\nESP-IDF：%s\nMAC：%s\n\n屏幕：360 × 360\nFlash：16 MB\nPSRAM：8 MB\n\n底部上滑返回桌面\n设置中统一配置 Wi-Fi", esp_app_get_description()->version, esp_app_get_description()->idf_ver, status.mac[0] ? status.mac : "正在读取");
    label(body, text);
    result_ = label(body, "");
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
    } else if (kind_ == Kind::Settings) {
        snprintf(text, sizeof(text), "%s%s%s\n%s", SystemService::stateText(status.state), status.ssid[0] ? " · " : "", status.ssid, status.message);
        lv_label_set_text(message_, text);
        lv_label_set_text(lv_obj_get_child(scan_button_, 0), status.scanning ? "正在扫描…" : "扫描 Wi-Fi");
        if (!status.ready || status.scanning || status.connection_pending || status.state == SystemService::NetworkState::Connecting) lv_obj_add_state(scan_button_, LV_STATE_DISABLED);
        else lv_obj_remove_state(scan_button_, LV_STATE_DISABLED);
        if (scan_revision_ != status.scan_revision) updateScanList(status);
        const auto bluetooth = BluetoothService::instance().snapshot();
        snprintf(text, sizeof(text), "%s\n名称：Codex Micro\n地址：%s\n%s", !bluetooth.ready ? "服务未就绪" : !bluetooth.enabled ? "已关闭" : bluetooth.connected ? "已连接电脑" : bluetooth.advertising ? "等待电脑配对/连接" : "准备广播", bluetooth.address[0] ? bluetooth.address : "--", bluetooth.message);
        lv_label_set_text(bluetooth_label_, text);
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
    } else if (kind_ == Kind::About) {
        const auto sample = BluetoothService::instance().snapshot().battery;
        formatBatteryInfo(text, sizeof(text), sample, static_cast<uint32_t>(esp_timer_get_time() / 1000));
        lv_label_set_text(battery_label_, text);
        snprintf(text, sizeof(text), "内部空闲：%u KB\n内部最大块：%u KB\nPSRAM 空闲：%u KB",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        lv_label_set_text(result_, text);
    }
}

void LauncherApp::updateScanList(const SystemService::Snapshot &status)
{
    scan_revision_ = status.scan_revision;
    listed_ap_count_ = status.ap_count;
    memcpy(listed_aps_, status.aps, sizeof(listed_aps_));
    lv_obj_clean(network_list_);
    for (int i = 0; i < status.ap_count; ++i) {
        char text[96];
        snprintf(text, sizeof(text), "%.32s  %d dBm%s", status.aps[i].ssid, status.aps[i].rssi, status.aps[i].secured ? " · 加密" : " · 开放");
        auto *obj = button(network_list_, text, onNetworkSelected, this, Scan);
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
    hideConnectionForm();
    secured_ = secured;
    form_ = lv_obj_create(root_);
    lv_obj_add_flag(form_, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(form_, 284, 276);
    lv_obj_align(form_, LV_ALIGN_CENTER, 0, 16);
    column(form_, 4);
    lv_obj_set_style_pad_all(form_, 14, 0);
    lv_obj_set_style_radius(form_, 12, 0);
    label(form_, "连接 Wi-Fi");
    ssid_input_ = lv_textarea_create(form_);
    lv_obj_set_size(ssid_input_, LV_PCT(100), 36);
    lv_obj_set_style_pad_all(ssid_input_, 4, 0);
    lv_textarea_set_one_line(ssid_input_, true);
    lv_textarea_set_max_length(ssid_input_, 32);
    lv_textarea_set_placeholder_text(ssid_input_, "网络名称 SSID");
    lv_textarea_set_text(ssid_input_, ssid);
    lv_obj_add_event_cb(ssid_input_, onTextFocused, LV_EVENT_FOCUSED, this);
    password_input_ = lv_textarea_create(form_);
    lv_obj_set_size(password_input_, LV_PCT(100), 36);
    lv_obj_set_style_pad_all(password_input_, 4, 0);
    lv_textarea_set_one_line(password_input_, true);
    lv_textarea_set_max_length(password_input_, 64);
    lv_textarea_set_password_mode(password_input_, true);
    lv_textarea_set_placeholder_text(password_input_, secured ? "密码，至少 8 位" : "开放网络，无需密码");
    lv_obj_add_event_cb(password_input_, onTextFocused, LV_EVENT_FOCUSED, this);
    if (!secured) lv_obj_add_state(password_input_, LV_STATE_DISABLED);
    auto *row = lv_obj_create(form_);
    lv_obj_set_size(row, LV_PCT(100), 36);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    auto *cancel = button(row, "取消", onAction, this, Cancel);
    lv_obj_set_size(cancel, LV_PCT(46), 34);
    lv_obj_align(cancel, LV_ALIGN_LEFT_MID, 0, 0);
    auto *connect = button(row, "连接", onAction, this, Connect);
    lv_obj_set_size(connect, LV_PCT(46), 34);
    lv_obj_align(connect, LV_ALIGN_RIGHT_MID, 0, 0);
    keyboard_ = lv_keyboard_create(form_);
    lv_obj_set_width(keyboard_, LV_PCT(100));
    lv_obj_set_flex_grow(keyboard_, 1);
    lv_obj_set_style_min_height(keyboard_, 100, 0);
    lv_keyboard_set_textarea(keyboard_, secured && ssid[0] ? password_input_ : ssid_input_);
    lv_obj_add_event_cb(keyboard_, onKeyboard, LV_EVENT_READY, this);
    lv_obj_add_event_cb(keyboard_, onKeyboard, LV_EVENT_CANCEL, this);
}

void LauncherApp::hideConnectionForm()
{
    if (password_input_) lv_textarea_set_text(password_input_, "");
    if (form_) lv_obj_delete(form_);
    form_ = ssid_input_ = password_input_ = keyboard_ = nullptr;
}

void LauncherApp::submitConnection()
{
    const char *ssid = lv_textarea_get_text(ssid_input_);
    const char *password = secured_ ? lv_textarea_get_text(password_input_) : "";
    const size_t password_length = strlen(password);
    bool hex_psk = password_length == 64;
    for (size_t i = 0; hex_psk && i < password_length; ++i) {
        if (!((password[i] >= '0' && password[i] <= '9') || (password[i] >= 'a' && password[i] <= 'f') || (password[i] >= 'A' && password[i] <= 'F'))) hex_psk = false;
    }
    if (!ssid[0] || strlen(ssid) > 32 || (secured_ && (password_length < 8 || (password_length == 64 && !hex_psk)))) {
        lv_label_set_text(lv_obj_get_child(form_, 0), "检查名称或密码长度");
        return;
    }
    if (!SystemService::instance().connect(ssid, password)) {
        lv_label_set_text(lv_obj_get_child(form_, 0), "服务忙，请稍后重试");
        return;
    }
    hideConnectionForm();
    refresh();
}

void LauncherApp::onTextFocused(lv_event_t *event)
{
    auto *self = static_cast<LauncherApp *>(lv_event_get_user_data(event));
    if (self->keyboard_) lv_keyboard_set_textarea(self->keyboard_, lv_event_get_target_obj(event));
}

void LauncherApp::onKeyboard(lv_event_t *event)
{
    auto *self = static_cast<LauncherApp *>(lv_event_get_user_data(event));
    if (lv_event_get_code(event) == LV_EVENT_READY) self->submitConnection();
    else self->hideConnectionForm();
}

void LauncherApp::onBrightness(lv_event_t *event)
{
    auto *self = static_cast<LauncherApp *>(lv_event_get_user_data(event));
    const int value = lv_slider_get_value(lv_event_get_target_obj(event));
    if (lv_event_get_code(event) == LV_EVENT_VALUE_CHANGED) {
        const esp_err_t err = bsp_display_brightness_set(value);
        char text[64];
        snprintf(text, sizeof(text), err == ESP_OK ? "屏幕亮度：%d%%" : "亮度调整失败：%d", value);
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
    case Home: self->notifyCoreClosed(); return;
    case Scan:
        if (!service.scan()) lv_label_set_text(self->message_, "正在连接或扫描，请稍后重试");
        break;
    case Manual: self->showConnectionForm("", true); break;
    case Forget:
        lv_obj_set_user_data(target, reinterpret_cast<void *>(ForgetConfirm));
        lv_label_set_text(lv_obj_get_child(target, 0), "确认忘记？再次点击");
        break;
    case ForgetConfirm:
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
        lv_obj_set_user_data(target, reinterpret_cast<void *>(PairConfirm));
        lv_label_set_text(lv_obj_get_child(target, 0), "确认清除旧配对？再次点击");
        break;
    case PairConfirm:
        if (!BluetoothService::instance().pairAgain()) lv_label_set_text(self->bluetooth_label_, "蓝牙服务忙，请稍后重试");
        lv_obj_set_user_data(target, reinterpret_cast<void *>(Pair));
        lv_label_set_text(lv_obj_get_child(target, 0), "重新配对");
        break;
    default: break;
    }
}
