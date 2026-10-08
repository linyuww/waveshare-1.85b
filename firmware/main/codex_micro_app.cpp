#include "codex_micro_app.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include "bluetooth_service.hpp"
#include "bsp/esp-bsp.h"
#include "dashboard_ui.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "system_service.hpp"
#include "ui_assets.h"

namespace {
constexpr char TAG[] = "codex_app";
uint32_t nowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
bool expired(uint32_t now, uint32_t until) { return static_cast<int32_t>(now - until) >= 0; }
}

CodexMicroApp::CodexMicroApp() : App("Codex Micro", &icon_codex, true, false, false) {}

bool CodexMicroApp::run()
{
    root_ = lv_screen_active();
    if (!canvas_.begin()) {
        auto *label = lv_label_create(root_);
        lv_obj_set_style_text_font(label, &ui_font, 0);
        lv_label_set_text(label, "仪表盘内存不足，请返回重试");
        lv_obj_center(label);
        return true;
    }
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_set_style_bg_color(root_, lv_color_hex(0x101923), 0);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);
    frame_.header.magic = LV_IMAGE_HEADER_MAGIC;
    frame_.header.cf = LV_COLOR_FORMAT_RGB565;
    frame_.header.w = 360;
    frame_.header.h = 360;
    frame_.header.stride = 720;
    frame_.data_size = 360 * 360 * 2;
    frame_.data = reinterpret_cast<const uint8_t *>(canvas_.pixels());
    image_ = lv_image_create(root_);
    lv_image_set_src(image_, &frame_);
    lv_obj_remove_flag(image_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(image_);
    auto *home = lv_button_create(root_);
    lv_obj_set_size(home, 44, 30);
    lv_obj_align(home, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_pad_all(home, 0, 0);
    lv_obj_set_style_radius(home, 10, 0);
    lv_obj_set_style_bg_opa(home, LV_OPA_80, 0);
    auto *icon = lv_label_create(home);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_18, 0);
    lv_label_set_text(icon, LV_SYMBOL_HOME);
    lv_obj_center(icon);
    lv_obj_add_event_cb(home, onHome, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(root_, onTouch, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(root_, onTouch, LV_EVENT_PRESSING, this);
    lv_obj_add_event_cb(root_, onTouch, LV_EVENT_RELEASED, this);
    lv_obj_add_event_cb(root_, onTouch, LV_EVENT_PRESS_LOST, this);
    gpio_config_t button = {};
    button.pin_bit_mask = 1ULL << GPIO_NUM_0;
    button.mode = GPIO_MODE_INPUT;
    button.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&button));
    timer_ = lv_timer_create(onTimer, 50, this);
    resume();
    return true;
}

void CodexMicroApp::restoreBrightness()
{
    const int value = SystemService::instance().snapshot().brightness;
    const esp_err_t err = bsp_display_brightness_set(value);
    if (err != ESP_OK) ESP_LOGW(TAG, "Brightness: %s", esp_err_to_name(err));
    brightness_ = value;
}

void CodexMicroApp::releaseControls()
{
    if (!BluetoothService::instance().releaseControls()) ESP_LOGW(TAG, "Control release queue full");
    tracking_ = send_ = power_hold_ = false;
    boot_.reset(gpio_get_level(GPIO_NUM_0) == 0, nowMs());
    direction_ = touch_gesture::Direction::None;
    agent_ = -1;
}

bool CodexMicroApp::back() { return notifyCoreClosed(); }
bool CodexMicroApp::pause()
{
    if (!active_) return true;
    active_ = false;
    if (timer_) lv_timer_pause(timer_);
    releaseControls();
    sleeping_ = false;
    restoreBrightness();
    return true;
}
bool CodexMicroApp::resume()
{
    if (!timer_) return true;
    active_ = true;
    sleeping_ = consume_touch_ = false;
    activity_at_ = nowMs();
    boot_.reset(gpio_get_level(GPIO_NUM_0) == 0, activity_at_);
    voice_until_ = 0;
    revision_ = UINT32_MAX;
    restoreBrightness();
    lv_timer_resume(timer_);
    render();
    return true;
}
bool CodexMicroApp::close()
{
    pause();
    if (timer_) lv_timer_delete(timer_);
    root_ = image_ = nullptr;
    timer_ = nullptr;
    return true;
}
bool CodexMicroApp::cleanResource() { canvas_.release(); frame_.data = nullptr; return true; }
void CodexMicroApp::onHome(lv_event_t *event) { static_cast<CodexMicroApp *>(lv_event_get_user_data(event))->back(); }
void CodexMicroApp::onTimer(lv_timer_t *timer) { static_cast<CodexMicroApp *>(lv_timer_get_user_data(timer))->tick(); }

bool CodexMicroApp::wake()
{
    activity_at_ = nowMs();
    const bool was_sleeping = sleeping_;
    sleeping_ = false;
    const int desired = SystemService::instance().snapshot().brightness;
    if (brightness_ != desired) restoreBrightness();
    return !was_sleeping;
}

void CodexMicroApp::onTouch(lv_event_t *event)
{
    auto *self = static_cast<CodexMicroApp *>(lv_event_get_user_data(event));
    if (!self->active_ || self->root_ != lv_screen_active() || lv_event_get_target_obj(event) != self->root_) return;
    lv_point_t point = {};
    if (auto *input = lv_indev_active()) lv_indev_get_point(input, &point);
    switch (lv_event_get_code(event)) {
    case LV_EVENT_PRESSED:
        self->consume_touch_ = !self->wake();
        if (!self->consume_touch_) self->press(point.x, point.y);
        break;
    case LV_EVENT_PRESSING:
        if (!self->consume_touch_) self->move(point.x, point.y);
        break;
    case LV_EVENT_RELEASED:
        if (!self->consume_touch_) self->release();
        self->consume_touch_ = false;
        break;
    case LV_EVENT_PRESS_LOST:
        self->releaseControls();
        self->consume_touch_ = false;
        break;
    default: break;
    }
}

void CodexMicroApp::press(int x, int y)
{
    tracking_ = true;
    start_x_ = x;
    start_y_ = y;
    touch_at_ = nowMs();
    power_hold_ = false;
    direction_ = touch_gesture::Direction::None;
    agent_ = dashboard::agentAtPoint(x, y);
    send_ = agent_ < 0 && dashboard::sendAtPoint(x, y);
    render();
}

void CodexMicroApp::move(int x, int y)
{
    if (!tracking_ || power_hold_ || direction_ != touch_gesture::Direction::None) return;
    auto direction = touch_gesture::classifySwipe(x - start_x_, y - start_y_, 40);
    if (direction == touch_gesture::Direction::None) return;
    // Reserve the bottom edge for the desktop's existing Home gesture.
    if (start_y_ >= 330) { releaseControls(); return; }
    agent_ = -1;
    send_ = false;
    direction_ = direction;
    BluetoothService::instance().joystick(direction, true);
    render();
}

void CodexMicroApp::release()
{
    if (!tracking_) return;
    auto &service = BluetoothService::instance();
    if (direction_ != touch_gesture::Direction::None) service.joystick(direction_, false);
    else if (!power_hold_) {
        if (agent_ >= 0) service.pulse(static_cast<BluetoothService::Key>(agent_));
        else if (send_) service.pulse(BluetoothService::Key::Send);
    }
    tracking_ = send_ = power_hold_ = false;
    agent_ = -1;
    direction_ = touch_gesture::Direction::None;
    if (!sleeping_) render();
}

void CodexMicroApp::updateButton()
{
    const uint32_t now = nowMs();
    const bool pulsed = boot_.update(gpio_get_level(GPIO_NUM_0) == 0, now,
        [this] { wake(); },
        [](bool pressed) {
            const bool accepted = BluetoothService::instance().holdMic(pressed);
            if (!pressed && !accepted) BluetoothService::instance().releaseControls();
            return accepted;
        },
        [] { return BluetoothService::instance().pulse(BluetoothService::Key::Voice); });
    if (pulsed) voice_until_ = now + 900;
}

void CodexMicroApp::tick()
{
    if (!active_) return;
    if (!root_ || root_ != lv_screen_active()) { pause(); return; }
    updateButton();
    const uint32_t now = nowMs();
    if (tracking_ || boot_.pressed()) activity_at_ = now;
    if (tracking_ && send_ && direction_ == touch_gesture::Direction::None && now - touch_at_ >= 2000) power_hold_ = true;
    if (power_hold_ && now - touch_at_ >= 6000) {
        releaseControls();
        sleeping_ = true;
        brightness_ = 0;
        bsp_display_brightness_set(0);
    }
    auto status = BluetoothService::instance().snapshot();
    const bool docked = status.external_power;
    const uint32_t idle = now - activity_at_;
    if (!sleeping_ && idle >= (docked ? 1800000U : 300000U)) {
        sleeping_ = true;
        brightness_ = 0;
        bsp_display_brightness_set(0);
    } else if (!sleeping_ && idle >= (docked ? 600000U : 120000U)) {
        const int dimmed = std::min(18, SystemService::instance().snapshot().brightness);
        if (brightness_ != dimmed) { bsp_display_brightness_set(dimmed); brightness_ = dimmed; }
    }
    if (sleeping_ && status.completion_at && now - status.completion_at < 1000) wake();
    if (!sleeping_ && (status.revision != revision_ || now - rendered_at_ >= 1000 || boot_.mic() || voice_until_ || power_hold_)) render();
}

void CodexMicroApp::render()
{
    if (!image_ || !active_) return;
    const uint32_t now = nowMs();
    const auto status = BluetoothService::instance().snapshot();
    const auto &codex = status.codex;
    connection_health::Input input;
    input.bleConnected = status.connected && status.enabled;
    input.hostRpcObserved = codex.hostRpcObserved;
    input.lastHostRpcAtMs = codex.lastHostRpcAtMs;
    input.quotaWaitingSinceMs = status.quota_waiting_since;
    input.quotaAvailable = codex.quota.available;
    input.quotaReceivedAtMs = codex.quota.restored ? now - 180001 : codex.quota.receivedAtMs;
    const auto health = connection_health::evaluate(input, now, 300000, 180000);
    dashboard::State ui;
    ui.linkHealth = health.link == connection_health::Link::CodexLive ? dashboard::LinkHealth::CodexLive : health.link == connection_health::Link::BleOnly ? dashboard::LinkHealth::BleOnly : dashboard::LinkHealth::Offline;
    ui.batteryPercent = status.battery_percent;
    ui.externalPower = status.external_power;
    ui.quotaAvailable = codex.quota.available;
    ui.quotaStale = health.quota == connection_health::Quota::Stale;
    ui.fiveHourRemainingPercent = codex.quota.fiveHourRemainingPercent;
    ui.weeklyRemainingPercent = codex.quota.weeklyRemainingPercent;
    const uint32_t elapsed = (now - codex.quota.receivedAtMs) / 1000;
    if (!codex.quota.restored && elapsed < codex.quota.fiveHourResetInSeconds) ui.fiveHourResetInSeconds = codex.quota.fiveHourResetInSeconds - elapsed;
    ui.timeValid = SystemService::timeValid();
    if (ui.timeValid) {
        const time_t time_now = time(nullptr);
        tm local = {};
        localtime_r(&time_now, &local);
        strftime(ui.clock, sizeof(ui.clock), "%H:%M", &local);
        strftime(ui.date, sizeof(ui.date), "%d %b %a", &local);
        night_ = theme::isNight(local.tm_hour);
    }
    ui.night = night_;
    ui.micPressed = boot_.mic();
    if (voice_until_ && expired(now, voice_until_)) voice_until_ = 0;
    ui.voicePressed = voice_until_ != 0;
    ui.sendPressed = send_ && !power_hold_;
    ui.activeTouchAgent = agent_;
    ui.swipeDirection = static_cast<int>(direction_);
    ui.completedAgent = status.completion_at && now - status.completion_at < 5000 ? status.completed_agent : -1;
    if (power_hold_) {
        ui.powerOverlay = dashboard::PowerOverlay::HoldToPowerOff;
        ui.powerHoldProgress = std::clamp((now - touch_at_ - 2000) / 4000.0f, 0.0f, 1.0f);
    }
    for (int i = 0; i < 6; ++i) {
        ui.threads[i].color = codex.threads[i].color;
        ui.threads[i].brightness = codex.threads[i].brightness;
        ui.threads[i].focused = !strcmp(codex.threads[i].effect, "breath") || codex.threads[i].speed > 0.01f;
    }
    dashboard::render(canvas_, ui);
    // Original canvas is panel-endian RGB565; LVGL expects native-endian pixels.
    for (int i = 0; i < 360 * 360; ++i) canvas_.pixels()[i] = gfx::swap16(canvas_.pixels()[i]);
    lv_obj_invalidate(image_);
    rendered_at_ = now;
    revision_ = status.revision;
}
