#include "audio_apps.hpp"
#include "codex_micro_app.hpp"
#include "bluetooth_service.hpp"
#include "system_service.hpp"
#include "ui_app_shell.hpp"
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

bool preview_boot_pressed = false;
namespace {
uint16_t framebuffer[360 * 360];
uint16_t pixels[360 * 360];
AssistantService::Config configuration;
AssistantService::Snapshot voice;
unsigned start_calls = 0;
unsigned stop_calls = 0;
unsigned toggle_calls = 0;
unsigned save_calls = 0;
unsigned listen_calls = 0, finish_calls = 0;
uint32_t next_boot_session = 0, boot_session = 0;
bool accept_boot = true, accept_mic = true;
unsigned codex_voices = 0, codex_holds = 0, codex_releases = 0;
void flush(lv_display_t *display, const lv_area_t *area, uint8_t *data)
{
    auto *source = reinterpret_cast<uint16_t *>(data);
    for (int vertical = area->y1; vertical <= area->y2; ++vertical)
        for (int horizontal = area->x1; horizontal <= area->x2; ++horizontal) pixels[vertical * 360 + horizontal] = *source++;
    lv_display_flush_ready(display);
}
void advance()
{
    lv_tick_inc(120);
    lv_timer_handler();
}
lv_obj_t *findButton(lv_obj_t *parent, const char *text)
{
    for (uint32_t index = 0; index < lv_obj_get_child_count(parent); ++index) {
        auto *child = lv_obj_get_child(parent, index);
        if (lv_obj_check_type(child, &lv_label_class) && strcmp(lv_label_get_text(child), text) == 0 &&
            lv_obj_check_type(parent, &lv_button_class)) return parent;
        if (auto *found = findButton(child, text)) return found;
    }
    return nullptr;
}
lv_obj_t *findInput(lv_obj_t *parent, const char *placeholder)
{
    for (uint32_t index = 0; index < lv_obj_get_child_count(parent); ++index) {
        auto *child = lv_obj_get_child(parent, index);
        if (lv_obj_check_type(child, &lv_textarea_class) && strcmp(lv_textarea_get_placeholder_text(child), placeholder) == 0) return child;
        if (auto *found = findInput(child, placeholder)) return found;
    }
    return nullptr;
}
lv_obj_t *findLabel(lv_obj_t *parent, const char *text)
{
    for (uint32_t index = 0; index < lv_obj_get_child_count(parent); ++index) {
        auto *child = lv_obj_get_child(parent, index);
        if (lv_obj_check_type(child, &lv_label_class) && strcmp(lv_label_get_text(child), text) == 0) return child;
        if (auto *found = findLabel(child, text)) return found;
    }
    return nullptr;
}
lv_obj_t *findAvatar(lv_obj_t *parent)
{
    for (uint32_t index = 0; index < lv_obj_get_child_count(parent); ++index) {
        auto *child = lv_obj_get_child(parent, index);
        if (lv_obj_check_type(child, &lv_image_class) && lv_obj_has_flag(child, LV_OBJ_FLAG_CLICKABLE)) return child;
        if (auto *found = findAvatar(child)) return found;
    }
    return nullptr;
}
void save(const std::string &path)
{
    lv_obj_update_layout(lv_screen_active());
    lv_refr_now(nullptr);
    auto *file = fopen(path.c_str(), "wb");
    assert(file);
    fprintf(file, "P6\n360 360\n255\n");
    for (const uint16_t pixel : pixels) {
        const unsigned char rgb[] = {static_cast<unsigned char>(((pixel >> 11) & 31) * 255 / 31),
            static_cast<unsigned char>(((pixel >> 5) & 63) * 255 / 63), static_cast<unsigned char>((pixel & 31) * 255 / 31)};
        fwrite(rgb, 1, 3, file);
    }
    fclose(file);
}
}

AssistantService &AssistantService::instance() { static AssistantService service; return service; }
AssistantService::Config AssistantService::config() { return configuration; }
AssistantService::Snapshot AssistantService::snapshot() { return voice; }
bool AssistantService::saveConfig(const Config &value) { configuration = value; ++save_calls; return true; }
bool AssistantService::start() { ++start_calls; return true; }
bool AssistantService::stop() { ++stop_calls; return true; }
bool AssistantService::toggleChat() { ++toggle_calls; return true; }

uint32_t AssistantService::beginBootListening() {
    if (!accept_boot) return 0;
    ++listen_calls;
    return boot_session = ++next_boot_session;
}
void AssistantService::finishBootListening(uint32_t session) {
    assert(session && session == boot_session);
    ++finish_calls;
    boot_session = 0;
}
SystemService &SystemService::instance() { static SystemService service; return service; }
SystemService::Snapshot SystemService::snapshot() { Snapshot result; result.ready = true; result.state = NetworkState::Connected; return result; }
const char *SystemService::stateText(NetworkState state) { return state == NetworkState::Connected ? "已连接" : "未连接"; }
bool SystemService::timeValid() { return false; }
BluetoothService &BluetoothService::instance() { static BluetoothService service; return service; }
BluetoothService::Snapshot BluetoothService::snapshot() { return {}; }
bool BluetoothService::pulse(Key key) { if (key == Key::Voice) ++codex_voices; return true; }
bool BluetoothService::holdMic(bool pressed) {
    if (!accept_mic) return false;
    if (pressed) ++codex_holds; else ++codex_releases;
    return true;
}
bool BluetoothService::releaseControls() { ++codex_releases; return true; }
bool BluetoothService::joystick(touch_gesture::Direction, bool) { return true; }

int main(int count, char **arguments)
{
    assert(count == 2);
    static_assert(offsetof(AssistantService::Config, volume) == 1280);
    strlcpy(configuration.reserved_music_url, "http://saved-host/music", sizeof(configuration.reserved_music_url));
    const std::string output = arguments[1];
    lv_init();
    auto *display = lv_display_create(360, 360);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, framebuffer, nullptr, sizeof(framebuffer), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush);
    auto *screen = lv_screen_active();
    for (unsigned cycle = 0; cycle < 100; ++cycle) {
        AudioApp assistant;
        voice = {};
        voice.connected = true;
        voice.listening = true;
        strlcpy(voice.recognized, "你好，小智", sizeof(voice.recognized));
        assert(assistant.run());
        assert(start_calls == cycle + 1);
        auto *avatar = findAvatar(screen);
        assert(avatar);
        for (const char *removed : {"开始对话", "打断并聆听", "说完了", "停止小智", "共享设置", "播放"}) assert(!findButton(screen, removed));
        assert(!findInput(screen, "歌名"));
        lv_obj_send_event(avatar, LV_EVENT_CLICKED, nullptr);
        assert(toggle_calls == cycle + 1);
        preview_boot_pressed = true;
        advance();
        advance();
        assert(toggle_calls == cycle + 1);
        preview_boot_pressed = false;
        advance();
        advance();
        assert(toggle_calls == cycle + 1);
        assert(listen_calls == cycle + 1 && finish_calls == cycle + 1);
        assert(findLabel(screen, "正在聆听"));
        assert(findLabel(screen, "你好，小智"));
        assert(findLabel(screen, "我"));
        voice.listening = false;
        voice.speaking = true;
        strlcpy(voice.reply, "你好，我是小智。", sizeof(voice.reply));
        advance();
        assert(findLabel(screen, "正在说话"));
        assert(findLabel(screen, "你好，我是小智。"));
        assert(findLabel(screen, "小智"));
        assert(!findLabel(screen, "你好，小智"));
        if (cycle == 0) {
            lv_obj_set_style_pad_bottom(screen, 96, 0);
            lv_obj_update_layout(screen);
            auto *hint = findLabel(screen, "按住 BOOT 说话，松开发送");
            lv_area_t hint_area;
            lv_obj_get_coords(hint, &hint_area);
            assert(hint_area.y2 < 264);
            lv_obj_set_style_pad_bottom(screen, 60, 0);
            save(output + "/assistant.ppm");
            auto *status = findLabel(screen, "正在说话");
            const char *unchanged = lv_label_get_text(status);
            lv_mem_monitor_t before = {};
            lv_mem_monitor(&before);
            for (unsigned tick = 0; tick < 10000; ++tick) advance();
            assert(lv_label_get_text(status) == unchanged);
            lv_mem_monitor_t after = {};
            lv_mem_monitor(&after);
            assert(after.free_size + 1024 >= before.free_size && lv_mem_test() == LV_RESULT_OK);
            // Replies remain readable beyond the visible card; unchanged refresh
            // must preserve the reader's scroll position.
            const char *long_reply = "你好，我是小智。\n你好，我是小智。\n你好，我是小智。\n你好，我是小智。\n你好，我是小智。\n你好，我是小智。";
            strlcpy(voice.reply, long_reply, sizeof(voice.reply));
            advance();
            lv_obj_update_layout(screen);
            auto *reply_label = findLabel(screen, long_reply);
            assert(reply_label && lv_label_get_long_mode(reply_label) == LV_LABEL_LONG_WRAP);
            auto *card = lv_obj_get_parent(reply_label);
            assert(lv_obj_get_scroll_bottom(card) > 0);
            lv_obj_scroll_to_y(card, 24, LV_ANIM_OFF);
            const int scroll = lv_obj_get_scroll_y(card);
            assert(scroll > 0);
            advance();
            assert(lv_obj_get_scroll_y(card) == scroll);
            save(output + "/long-reply.ppm");
            strlcpy(voice.reply, "你好，我是小智。", sizeof(voice.reply));
            advance();
            assert(lv_obj_get_scroll_y(card) == 0);
            auto *other_screen = lv_obj_create(nullptr);
            lv_screen_load(other_screen);
            strlcpy(voice.reply, "后台不更新页面", sizeof(voice.reply));
            advance();
            assert(findLabel(screen, "你好，我是小智。"));
            lv_screen_load(screen);
            assert(assistant.resume());
            advance();
            assert(findLabel(screen, "后台不更新页面"));
            lv_obj_delete(other_screen);
            strlcpy(voice.activation, "123456", sizeof(voice.activation));
            voice.listening = voice.speaking = false;
            strlcpy(voice.message, "请绑定设备", sizeof(voice.message));
            advance();
            auto *code = findLabel(screen, "激活码：123456");
            assert(code && !lv_obj_has_flag(code, LV_OBJ_FLAG_HIDDEN));
            assert(lv_obj_has_flag(findLabel(screen, "后台不更新页面"), LV_OBJ_FLAG_HIDDEN));
            save(output + "/activation.ppm");
        }
        assert(assistant.close());
        assert(assistant.close());
        assert(stop_calls == 0);
        lv_obj_clean(screen);
        advance();
        assert(toggle_calls == cycle + 1);
        auto shell = createLauncherAppShell(screen, "共享设置");
        buildSharedAudioSettings(shell.body);
        auto *ota = findInput(screen, "https://.../ota/");
        assert(ota && !findInput(screen, "http://电脑IP:端口/music"));
        lv_textarea_set_text(ota, "https://example.com/ota/");
        auto *save_button = findButton(screen, "保存助手设置");
        assert(save_button);
        lv_obj_send_event(save_button, LV_EVENT_CLICKED, nullptr);
        assert(strcmp(configuration.ota_url, "https://example.com/ota/") == 0);
        assert(strcmp(configuration.reserved_music_url, "http://saved-host/music") == 0);
        assert(save_calls == cycle + 1);
        lv_obj_clean(screen);
        assert(lv_mem_test() == LV_RESULT_OK);
    }
    AudioApp destroyed;
    assert(destroyed.run());
    auto *replacement = lv_obj_create(nullptr);
    lv_screen_load(replacement);
    lv_obj_delete(screen);
    advance();
    assert(destroyed.close());
    assert(stop_calls == 0);
    assert(toggle_calls == 100);
    assert(lv_mem_test() == LV_RESULT_OK);
    lv_obj_clean(replacement);
    screen = replacement;
    AudioApp assistant;
    auto *other = lv_obj_create(nullptr);
    assert(assistant.run());
    // Entering while held must not begin or finish somebody else's recording.
    assert(assistant.pause());
    preview_boot_pressed = true;
    assert(assistant.resume());
    advance(); advance();
    preview_boot_pressed = false;
    advance(); advance();
    assert(listen_calls == 100 && finish_calls == 100);
    preview_boot_pressed = true;
    advance(); advance();
    assert(listen_calls == 101);
    assert(assistant.pause());
    assert(finish_calls == 101);
    assert(assistant.close());
    assert(finish_calls == 101);
    lv_obj_clean(screen);
    assert(assistant.run()); // still held: cannot continue the previous gesture
    advance(); advance();
    preview_boot_pressed = false;
    advance(); advance();
    assert(listen_calls == 101 && finish_calls == 101);
    // Screen changes without a lifecycle callback still finish exactly once.
    preview_boot_pressed = true;
    advance(); advance();
    assert(listen_calls == 102);
    lv_screen_load(other);
    advance(); advance();
    assert(finish_calls == 102);
    preview_boot_pressed = false;
    advance(); advance();
    preview_boot_pressed = true;
    advance(); advance();
    assert(listen_calls == 102 && finish_calls == 102);
    lv_screen_load(screen);
    assert(assistant.resume());
    preview_boot_pressed = false;
    advance(); advance();
    // A rejected press must not later finish an unrelated background session.
    accept_boot = false;
    preview_boot_pressed = true;
    advance(); advance();
    preview_boot_pressed = false;
    advance(); advance();
    assert(listen_calls == 102 && finish_calls == 102);
    accept_boot = true;
    preview_boot_pressed = true;
    advance(); advance();
    assert(listen_calls == 103);
    preview_boot_pressed = false;
    advance(); advance();
    assert(finish_calls == 103);
    assert(assistant.close());
    lv_obj_clean(screen);
    // Both production apps exist simultaneously; only the active screen routes BOOT.
    assert(assistant.run());
    assert(assistant.pause());
    lv_screen_load(other);
    CodexMicroApp codex;
    assert(codex.run());
    preview_boot_pressed = true;
    advance(); advance();
    preview_boot_pressed = false;
    advance(); advance();
    assert(codex_voices == 1 && listen_calls == 103);
    preview_boot_pressed = true;
    advance(); advance();
    for (int tick = 0; tick < 6; ++tick) advance();
    assert(codex_holds == 1 && codex_releases == 0);
    // Lost screen also releases Codex exactly once, without a short press.
    lv_screen_load(screen);
    assert(assistant.resume()); // still held by the gesture from Codex
    advance(); advance();
    assert(codex_releases == 1 && codex_voices == 1 && listen_calls == 103);
    preview_boot_pressed = false;
    advance(); advance();
    assert(finish_calls == 103);
    preview_boot_pressed = true;
    advance(); advance();
    assert(listen_calls == 104 && codex_holds == 1);
    assert(assistant.pause());
    assert(finish_calls == 104);
    lv_screen_load(other);
    assert(codex.resume()); // still held: must first release
    advance(); advance();
    preview_boot_pressed = false;
    advance(); advance();
    assert(codex_voices == 1 && codex_holds == 1);
    preview_boot_pressed = true;
    advance(); advance();
    accept_mic = false;
    for (int tick = 0; tick < 6; ++tick) advance();
    assert(codex_holds == 1);
    accept_mic = true;
    advance();
    assert(codex_holds == 2);
    accept_mic = false; // releasing into a full queue uses reliable releaseControls
    preview_boot_pressed = false;
    advance(); advance();
    assert(codex_releases == 2);
    accept_mic = true;
    assert(codex.pause());
    const unsigned releases_after_pause = codex_releases;
    assert(codex.close());
    assert(codex_releases == releases_after_pause);
    assert(codex.cleanResource());
    assert(assistant.close());
    assert(finish_calls == 104);
    lv_screen_load(screen);
    lv_obj_clean(screen);
    lv_obj_delete(other);
    puts("PASS: production Codex/audio routing, single releases, held entry/switch, queue failures and 100 cleanup cycles");
}
