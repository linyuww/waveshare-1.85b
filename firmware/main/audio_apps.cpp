#include "audio_apps.hpp"
#include <cstdio>
#include <cstring>
#include "ui_app_shell.hpp"
#include "driver/gpio.h"

namespace {
struct SettingsForm {
    lv_obj_t *ota;
    lv_obj_t *websocket;
    lv_obj_t *token;
    lv_obj_t *volume;
    lv_obj_t *message;
    lv_obj_t *keyboard;
};
lv_obj_t *label(lv_obj_t *parent, const char *text)
{
    auto *object = lv_label_create(parent);
    lv_obj_set_width(object, LV_PCT(100));
    lv_label_set_text(object, text);
    return object;
}
void updateLabel(lv_obj_t *object, const char *text)
{
    if (strcmp(lv_label_get_text(object), text) != 0) lv_label_set_text(object, text);
}
lv_obj_t *input(lv_obj_t *parent, const char *placeholder, const char *text, unsigned maximum)
{
    auto *object = lv_textarea_create(parent);
    lv_obj_set_width(object, LV_PCT(100));
    lv_textarea_set_one_line(object, true);
    lv_textarea_set_max_length(object, maximum);
    lv_textarea_set_placeholder_text(object, placeholder);
    lv_textarea_set_text(object, text);
    return object;
}
void settingsKeyboard(lv_event_t *event)
{
    auto *form = static_cast<SettingsForm *>(lv_event_get_user_data(event));
    if (lv_event_get_code(event) == LV_EVENT_READY || lv_event_get_code(event) == LV_EVENT_CANCEL) lv_obj_add_flag(form->keyboard, LV_OBJ_FLAG_HIDDEN);
}
void settingsFocus(lv_event_t *event)
{
    auto *form = static_cast<SettingsForm *>(lv_event_get_user_data(event));
    lv_keyboard_set_textarea(form->keyboard, lv_event_get_target_obj(event));
    lv_obj_remove_flag(form->keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_scroll_to_view(lv_event_get_target_obj(event), LV_ANIM_OFF);
}
void saveSettings(lv_event_t *event)
{
    auto *form = static_cast<SettingsForm *>(lv_event_get_user_data(event));
    auto config = AssistantService::instance().config();
    strlcpy(config.ota_url, lv_textarea_get_text(form->ota), sizeof(config.ota_url));
    strlcpy(config.websocket_url, lv_textarea_get_text(form->websocket), sizeof(config.websocket_url));
    strlcpy(config.token, lv_textarea_get_text(form->token), sizeof(config.token));
    config.volume = lv_slider_get_value(form->volume);
    const bool saved = AssistantService::instance().saveConfig(config);
    lv_label_set_text(form->message, saved ? "保存请求已提交，重连小智生效" : "地址无效或队列已满，请重试");
    lv_obj_add_flag(form->keyboard, LV_OBJ_FLAG_HIDDEN);
}
}

void buildSharedAudioSettings(lv_obj_t *body)
{
    auto *form = new SettingsForm{};
    const auto config = AssistantService::instance().config();
    label(body, "AI 助手与共享音频");
    label(body, "Wi-Fi 共用本页设置。Codex 提示音优先。");
    label(body, "激活服务地址");
    form->ota = input(body, "https://.../ota/", config.ota_url, 255);
    label(body, "自建 WebSocket（留空使用激活服务）");
    form->websocket = input(body, "wss://...", config.websocket_url, 255);
    label(body, "小智令牌（可留空）");
    form->token = input(body, "Token", config.token, 511);
    lv_textarea_set_password_mode(form->token, true);
    label(body, "共享音量（0 静音，100 最大）");
    form->volume = lv_slider_create(body);
    lv_obj_set_width(form->volume, LV_PCT(90));
    lv_slider_set_range(form->volume, 0, 100);
    lv_slider_set_value(form->volume, config.volume, LV_ANIM_OFF);
    auto *save = lv_button_create(body);
    lv_obj_set_width(save, LV_PCT(100));
    auto *caption = lv_label_create(save);
    lv_label_set_text(caption, "保存助手设置");
    lv_obj_center(caption);
    form->message = label(body, "服务地址和令牌仅保存在设备 NVS");
    form->keyboard = lv_keyboard_create(lv_screen_active());
    lv_obj_set_size(form->keyboard, 300, 150);
    lv_obj_align(form->keyboard, LV_ALIGN_BOTTOM_MID, 0, -35);
    lv_obj_set_style_text_font(form->keyboard, &lv_font_montserrat_18, 0);
    lv_obj_add_flag(form->keyboard, LV_OBJ_FLAG_HIDDEN);
    for (auto *field : {form->ota, form->websocket, form->token}) lv_obj_add_event_cb(field, settingsFocus, LV_EVENT_FOCUSED, form);
    lv_obj_add_event_cb(form->keyboard, settingsKeyboard, LV_EVENT_ALL, form);
    lv_obj_add_event_cb(save, saveSettings, LV_EVENT_CLICKED, form);
    lv_obj_add_event_cb(body, [](lv_event_t *event) {
        auto *form = static_cast<SettingsForm *>(lv_event_get_user_data(event));
        lv_obj_delete(form->keyboard);
        delete form;
    }, LV_EVENT_DELETE, form);
}

AudioApp::AudioApp() : App("小智助手", &icon_assistant, true, false, false) {}
bool AudioApp::run()
{
    root_ = lv_screen_active();
    lv_obj_add_event_cb(root_, onRootDeleted, LV_EVENT_DELETE, this);
    const auto shell = createLauncherAppShell(root_, "小智助手");
    lv_obj_add_event_cb(shell.home, onHome, LV_EVENT_CLICKED, this);
    lv_obj_remove_flag(shell.body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_row(shell.body, 8, 0);
    lv_obj_set_style_text_align(shell.body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_flex_align(shell.body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    auto *header = lv_obj_create(shell.body);
    lv_obj_set_size(header, LV_PCT(100), 80);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header, 12, 0);
    auto *avatar = lv_image_create(header);
    lv_image_set_src(avatar, &icon_assistant);
    lv_obj_add_flag(avatar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(avatar, onToggle, LV_EVENT_CLICKED, this);
    auto *details = lv_obj_create(header);
    lv_obj_set_size(details, 148, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(details, 0, 0);
    lv_obj_set_style_border_width(details, 0, 0);
    lv_obj_set_style_bg_opa(details, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(details, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(details, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(details, 8, 0);
    status_ = label(details, "正在连接");
    lv_obj_set_height(status_, 48);
    lv_label_set_long_mode(status_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(status_, lv_color_hex(0xAAB9CB), 0);
    auto *toggle_hint = label(details, "点头像切换");
    lv_obj_set_style_text_color(toggle_hint, lv_color_hex(0xAAB9CB), 0);
    activation_ = label(shell.body, "");
    lv_obj_add_flag(activation_, LV_OBJ_FLAG_HIDDEN);
    conversation_ = lv_obj_create(shell.body);
    lv_obj_set_width(conversation_, LV_PCT(100));
    lv_obj_set_height(conversation_, 0);
    lv_obj_set_flex_grow(conversation_, 1);
    lv_obj_set_style_bg_color(conversation_, lv_color_hex(0x1B293B), 0);
    lv_obj_set_style_text_color(conversation_, lv_color_hex(0xF1F5FA), 0);
    lv_obj_set_style_text_font(conversation_, &ui_font, 0);
    lv_obj_set_style_radius(conversation_, 16, 0);
    lv_obj_set_style_border_width(conversation_, 0, 0);
    lv_obj_set_style_pad_all(conversation_, 10, 0);
    lv_obj_set_style_pad_row(conversation_, 4, 0);
    lv_obj_set_flex_flow(conversation_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(conversation_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(conversation_, LV_SCROLLBAR_MODE_AUTO);
    speaker_ = label(conversation_, "小智");
    lv_obj_set_style_text_color(speaker_, lv_color_hex(0xBAA5FF), 0);
    transcript_ = label(conversation_, "");
    lv_label_set_long_mode(transcript_, LV_LABEL_LONG_WRAP);
    hint_ = label(shell.body, "按住 BOOT 说话，松开发送");
    lv_obj_set_style_text_color(hint_, lv_color_hex(0xAAB9CB), 0);
    gpio_config_t button_config = {};
    button_config.pin_bit_mask = 1ULL << GPIO_NUM_0;
    button_config.mode = GPIO_MODE_INPUT;
    button_config.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&button_config));
    timer_ = lv_timer_create(onTimer, 50, this);
    resume();
    return true;
}
bool AudioApp::back() { return notifyCoreClosed(); }
bool AudioApp::pause()
{
    const bool was_active = active_;
    active_ = false;
    if (timer_) lv_timer_pause(timer_);
    if (was_active) AssistantService::instance().stop();
    if (boot_session_) {
        AssistantService::instance().finishBootListening(boot_session_);
        boot_session_ = 0;
    }
    return true;
}
bool AudioApp::resume()
{
    if (!timer_ || !root_) return true;
    if (active_) return true;
    active_ = true;
    boot_.reset(gpio_get_level(GPIO_NUM_0) == 0, lv_tick_get());
    lv_timer_resume(timer_);
    if (!AssistantService::instance().start()) updateLabel(status_, "暂时无法开始，请点击重试");
    return true;
}
bool AudioApp::close()
{
    pause();
    if (root_) lv_obj_remove_event_cb_with_user_data(root_, onRootDeleted, this);
    if (timer_) lv_timer_delete(timer_);
    timer_ = nullptr;
    root_ = status_ = transcript_ = conversation_ = speaker_ = activation_ = hint_ = nullptr;
    return true;
}
void AudioApp::onRootDeleted(lv_event_t *event)
{
    auto *self = static_cast<AudioApp *>(lv_event_get_user_data(event));
    if (lv_event_get_target_obj(event) != self->root_) return;
    self->root_ = nullptr;
    self->close();
}
void AudioApp::onHome(lv_event_t *event)
{
    static_cast<AudioApp *>(lv_event_get_user_data(event))->notifyCoreClosed();
}
void AudioApp::onToggle(lv_event_t *event)
{
    auto *self = static_cast<AudioApp *>(lv_event_get_user_data(event));
    if (!self->active_ || self->root_ != lv_screen_active()) return;
    if (!AssistantService::instance().toggleChat()) updateLabel(self->status_, "操作未受理，请稍后重试");
}
void AudioApp::onTimer(lv_timer_t *timer)
{
    auto *self = static_cast<AudioApp *>(lv_timer_get_user_data(timer));
    if (!self->active_ || !self->root_ || !self->status_ || !self->transcript_) return;
    if (self->root_ != lv_screen_active()) { self->pause(); return; }
    const auto edge = self->boot_.update(gpio_get_level(GPIO_NUM_0) == 0, lv_tick_get());
    if (edge == BootButton::Edge::Press)
        self->boot_session_ = AssistantService::instance().beginBootListening();
    if (edge == BootButton::Edge::Release && self->boot_session_) {
        AssistantService::instance().finishBootListening(self->boot_session_);
        self->boot_session_ = 0;
    }
    self->refresh();
    if (self->boot_.pressed() && !self->boot_session_)
        updateLabel(self->status_, "操作未受理，请松开后重试");
}
void AudioApp::refresh()
{
    const auto assistant = AssistantService::instance().snapshot();
    updateLabel(status_, assistant.speaking ? "正在说话" : assistant.listening ? "正在聆听" : assistant.message);
    const uint32_t color = assistant.speaking ? 0xBAA5FF : assistant.listening ? 0x71DFBF : 0xAAB9CB;
    if (lv_color_to_u32(lv_obj_get_style_text_color(status_, LV_PART_MAIN)) != lv_color_to_u32(lv_color_hex(color)))
        lv_obj_set_style_text_color(status_, lv_color_hex(color), 0);
    if (assistant.activation[0]) {
        char text[80];
        snprintf(text, sizeof(text), "激活码：%s", assistant.activation);
        updateLabel(activation_, text);
        lv_obj_remove_flag(activation_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(conversation_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(transcript_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(activation_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(conversation_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(transcript_, LV_OBJ_FLAG_HIDDEN);
        const bool reply = assistant.reply[0] != 0;
        updateLabel(speaker_, reply ? "小智" : "我");
        const char *text = reply ? assistant.reply : assistant.recognized[0] ? assistant.recognized : "说点什么吧";
        if (strcmp(lv_label_get_text(transcript_), text) != 0) {
            lv_label_set_text(transcript_, text);
            lv_obj_scroll_to_y(conversation_, 0, LV_ANIM_OFF);
        }
    }
}
