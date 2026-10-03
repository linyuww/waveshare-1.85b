#include "audio_apps.hpp"
#include <cstdio>
#include <cstring>
#include "app_navigation.hpp"
#include "music_service.hpp"
#include "shared_audio.hpp"
#include "system_service.hpp"
#include "ui_app_shell.hpp"
#include "driver/gpio.h"

namespace {
enum Action { Home, Start, Listen, Finish, Stop, Settings, Play, Pause, Resume };
struct SettingsForm {
    lv_obj_t *root;
    lv_obj_t *ota;
    lv_obj_t *websocket;
    lv_obj_t *token;
    lv_obj_t *music;
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
lv_obj_t *input(lv_obj_t *parent, const char *placeholder, const char *text = "", unsigned maximum = 120)
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
    strlcpy(config.music_url, lv_textarea_get_text(form->music), sizeof(config.music_url));
    config.volume = lv_slider_get_value(form->volume);
    const bool saved = AssistantService::instance().saveConfig(config);
    lv_label_set_text(form->message, saved ? "保存请求已提交，重连小智生效" : "地址无效或队列已满，请重试");
    lv_obj_add_flag(form->keyboard, LV_OBJ_FLAG_HIDDEN);
}
}

void buildSharedAudioSettings(lv_obj_t *body)
{
    auto *form = new SettingsForm{};
    form->root = body;
    const auto config = AssistantService::instance().config();
    label(body, "AI 助手与共享音频");
    label(body, "Wi-Fi 共用本页设置。提示音优先于语音和音乐。");
    label(body, "激活服务地址");
    form->ota = input(body, "https://.../ota/", config.ota_url, 255);
    label(body, "自建 WebSocket（留空使用激活服务）");
    form->websocket = input(body, "wss://...", config.websocket_url, 255);
    label(body, "小智令牌（可留空）");
    form->token = input(body, "Token", config.token, 511);
    lv_textarea_set_password_mode(form->token, true);
    label(body, "音乐服务地址（与旧摆件 /resolve、/stream 兼容）");
    form->music = input(body, "http://电脑IP:端口/music", config.music_url, 255);
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
    for (auto *field : {form->ota, form->websocket, form->token, form->music}) lv_obj_add_event_cb(field, settingsFocus, LV_EVENT_FOCUSED, form);
    lv_obj_add_event_cb(form->keyboard, settingsKeyboard, LV_EVENT_ALL, form);
    lv_obj_add_event_cb(save, saveSettings, LV_EVENT_CLICKED, form);
    lv_obj_add_event_cb(body, [](lv_event_t *event) {
        auto *form = static_cast<SettingsForm *>(lv_event_get_user_data(event));
        lv_obj_delete(form->keyboard);
        delete form;
    }, LV_EVENT_DELETE, form);
}

AudioApp::AudioApp(Kind kind) : App(kind == Kind::Assistant ? "小智助手" : "音乐播放器", kind == Kind::Assistant ? &icon_assistant : &icon_music, true, false, false), kind_(kind) {}
lv_obj_t *AudioApp::button(lv_obj_t *parent, const char *text, int action)
{
    auto *object = lv_button_create(parent);
    lv_obj_set_width(object, LV_PCT(100));
    auto *caption = lv_label_create(object);
    lv_label_set_text(caption, text);
    lv_obj_center(caption);
    lv_obj_set_user_data(object, reinterpret_cast<void *>(static_cast<intptr_t>(action)));
    lv_obj_add_event_cb(object, onAction, LV_EVENT_CLICKED, this);
    return object;
}
bool AudioApp::run()
{
    root_ = lv_screen_active();
    lv_obj_add_event_cb(root_, onRootDeleted, LV_EVENT_DELETE, this);
    const auto shell = createLauncherAppShell(root_, kind_ == Kind::Assistant ? "小智助手" : "音乐播放器");
    lv_obj_set_user_data(shell.home, reinterpret_cast<void *>(static_cast<intptr_t>(Home)));
    lv_obj_add_event_cb(shell.home, onAction, LV_EVENT_CLICKED, this);
    status_ = label(shell.body, "");
    transcript_ = label(shell.body, "");
    if (kind_ == Kind::Assistant) {
        button(shell.body, "开始对话", Start);
        button(shell.body, "打断并聆听", Listen);
        button(shell.body, "说完了", Finish);
        button(shell.body, "停止小智", Stop);
        button(shell.body, "共享设置", Settings);
        label(shell.body, "BOOT 按下说话，松开发送。音乐待机时仍监听暂停命令。");
        gpio_config_t button_config = {};
        button_config.pin_bit_mask = 1ULL << GPIO_NUM_0;
        button_config.mode = GPIO_MODE_INPUT;
        button_config.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&button_config);
    } else {
        song_ = input(shell.body, "歌名");
        artist_ = input(shell.body, "歌手（可留空）");
        keyboard_ = lv_keyboard_create(root_);
        lv_obj_set_size(keyboard_, 300, 150);
        lv_obj_align(keyboard_, LV_ALIGN_BOTTOM_MID, 0, -35);
        lv_obj_set_style_text_font(keyboard_, &lv_font_montserrat_18, 0);
        lv_obj_add_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(song_, onFocus, LV_EVENT_FOCUSED, this);
        lv_obj_add_event_cb(artist_, onFocus, LV_EVENT_FOCUSED, this);
        lv_obj_add_event_cb(keyboard_, onKeyboard, LV_EVENT_ALL, this);
        button(shell.body, "播放", Play);
        button(shell.body, "暂停并返回小智", Pause);
        button(shell.body, "继续播放", Resume);
        button(shell.body, "共享设置", Settings);
    }
    timer_ = lv_timer_create(onTimer, 100, this);
    refresh();
    return true;
}
bool AudioApp::back() { return notifyCoreClosed(); }
bool AudioApp::close()
{
    if (root_) lv_obj_remove_event_cb_with_user_data(root_, onRootDeleted, this);
    if (timer_) lv_timer_delete(timer_);
    timer_ = nullptr;
    root_ = status_ = transcript_ = song_ = artist_ = keyboard_ = nullptr;
    boot_pressed_ = false;
    return true;
}
void AudioApp::onRootDeleted(lv_event_t *event)
{
    auto *self = static_cast<AudioApp *>(lv_event_get_user_data(event));
    if (lv_event_get_target_obj(event) != self->root_) return;
    self->root_ = nullptr;
    self->close();
}
void AudioApp::onAction(lv_event_t *event)
{
    auto *self = static_cast<AudioApp *>(lv_event_get_user_data(event));
    const int action = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(event))));
    auto &assistant = AssistantService::instance();
    bool result = true;
    switch (action) {
    case Home: self->notifyCoreClosed(); return;
    case Start: result = assistant.start(); break;
    case Listen: result = assistant.listen(); break;
    case Finish: result = assistant.finishListening(); break;
    case Stop: result = assistant.stop(); break;
    case Settings: result = AppNavigation::request(AppTarget::Settings); break;
    case Play:
        result = MusicService::instance().play(lv_textarea_get_text(self->song_), lv_textarea_get_text(self->artist_));
        lv_obj_add_flag(self->keyboard_, LV_OBJ_FLAG_HIDDEN);
        break;
    case Pause: MusicService::instance().pause(); break;
    case Resume: result = MusicService::instance().resume(); break;
    }
    if (!result) lv_label_set_text(self->status_, "操作未受理，请检查歌名、共享设置或服务状态");
}
void AudioApp::onFocus(lv_event_t *event)
{
    auto *self = static_cast<AudioApp *>(lv_event_get_user_data(event));
    lv_keyboard_set_textarea(self->keyboard_, lv_event_get_target_obj(event));
    lv_obj_remove_flag(self->keyboard_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_scroll_to_view(lv_event_get_target_obj(event), LV_ANIM_OFF);
}
void AudioApp::onKeyboard(lv_event_t *event)
{
    auto *self = static_cast<AudioApp *>(lv_event_get_user_data(event));
    if (lv_event_get_code(event) == LV_EVENT_READY || lv_event_get_code(event) == LV_EVENT_CANCEL) lv_obj_add_flag(self->keyboard_, LV_OBJ_FLAG_HIDDEN);
}
void AudioApp::onTimer(lv_timer_t *timer)
{
    auto *self = static_cast<AudioApp *>(lv_timer_get_user_data(timer));
    if (!self->root_ || !self->status_ || !self->transcript_ || self->root_ != lv_screen_active()) return;
    if (self->kind_ == Kind::Assistant && self->root_ == lv_screen_active()) {
        const bool pressed = gpio_get_level(GPIO_NUM_0) == 0;
        if (pressed && !self->boot_pressed_) AssistantService::instance().listen();
        if (!pressed && self->boot_pressed_) AssistantService::instance().finishListening();
        self->boot_pressed_ = pressed;
    }
    self->refresh();
}
void AudioApp::refresh()
{
    const auto music = MusicService::instance().snapshot();
    const bool music_active = music.state == MusicService::State::Playing || music.state == MusicService::State::Resolving;
    char text[900];
    if (kind_ == Kind::Assistant) {
        const auto network = SystemService::instance().snapshot();
        const auto assistant = AssistantService::instance().snapshot();
        snprintf(text, sizeof(text), "Wi-Fi：%s\n%s\n%s%s", SystemService::stateText(network.state), music_active ? "音乐播放中，小智待机并后台聆听" : assistant.message, assistant.activation[0] ? "激活码：" : "", assistant.activation);
        updateLabel(status_, text);
        snprintf(text, sizeof(text), "我：%s\n小智：%s", assistant.recognized[0] ? assistant.recognized : "--", assistant.reply[0] ? assistant.reply : "--");
        updateLabel(transcript_, text);
    } else {
        snprintf(text, sizeof(text), "%s\n音量：%d%%\nCodex 完成提示音优先", music.message, shared_audio::volume());
        updateLabel(status_, text);
        snprintf(text, sizeof(text), "%s\n%s\n%lu:%02lu", music.song[0] ? music.song : "未选择歌曲", music.artist,
            static_cast<unsigned long>(music.playback_ms / 60000), static_cast<unsigned long>(music.playback_ms / 1000 % 60));
        updateLabel(transcript_, text);
    }
}
