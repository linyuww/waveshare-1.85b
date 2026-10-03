#include "audio_apps.hpp"
#include "app_navigation.hpp"
#include "music_service.hpp"
#include "shared_audio.hpp"
#include "system_service.hpp"
#include "ui_app_shell.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

bool preview_boot_pressed = false;
namespace {
uint16_t framebuffer[360 * 360];
uint16_t pixels[360 * 360];
AssistantService::Config configuration;
AssistantService::Snapshot voice;
MusicService::Snapshot music;
unsigned start_calls = 0;
unsigned listen_calls = 0;
unsigned finish_calls = 0;
unsigned save_calls = 0;
unsigned play_calls = 0;
AppTarget destination = AppTarget::Codex;
bool online = true;
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
void click(const char *text)
{
    auto *button = findButton(lv_screen_active(), text);
    assert(button);
    lv_obj_send_event(button, LV_EVENT_CLICKED, nullptr);
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
bool AssistantService::stop() { return true; }
bool AssistantService::listen() { ++listen_calls; return true; }
bool AssistantService::finishListening() { ++finish_calls; return true; }
MusicService &MusicService::instance() { static MusicService service; return service; }
MusicService::Snapshot MusicService::snapshot() { return music; }
bool MusicService::play(const char *song, const char *) { ++play_calls; return song[0]; }
void MusicService::pause(bool return_to_assistant) { music.state = State::Paused; if (return_to_assistant) destination = AppTarget::Assistant; }
bool MusicService::resume() { music.state = State::Playing; return true; }
SystemService &SystemService::instance() { static SystemService service; return service; }
SystemService::Snapshot SystemService::snapshot() { Snapshot result; result.ready = true; result.state = online ? NetworkState::Connected : NetworkState::Offline; return result; }
const char *SystemService::stateText(NetworkState state) { return state == NetworkState::Connected ? "已连接" : "未连接"; }
bool AppNavigation::request(AppTarget target) { destination = target; return true; }
int shared_audio::volume() { return 60; }

int main(int count, char **arguments)
{
    assert(count == 2);
    const std::string output = arguments[1];
    lv_init();
    auto *display = lv_display_create(360, 360);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, framebuffer, nullptr, sizeof(framebuffer), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush);
    auto *screen = lv_screen_active();
    for (int cycle = 0; cycle < 100; ++cycle) {
        AudioApp assistant(AudioApp::Kind::Assistant);
        strlcpy(voice.recognized, "打开 Codex Micro", sizeof(voice.recognized));
        strlcpy(voice.reply, "可以打开工具，也可以播放或暂停音乐。", sizeof(voice.reply));
        assert(assistant.run());
        click("开始对话");
        preview_boot_pressed = true;
        advance();
        preview_boot_pressed = false;
        advance();
        click("共享设置");
        assert(destination == AppTarget::Settings);
        if (cycle == 0) save(output + "/assistant.ppm");
        assert(assistant.close());
        lv_obj_clean(screen);
        AudioApp player(AudioApp::Kind::Music);
        music.state = MusicService::State::Playing;
        strlcpy(music.song, "测试歌曲", sizeof(music.song));
        strlcpy(music.message, "正在播放，小智后台聆听暂停命令", sizeof(music.message));
        assert(player.run());
        if (cycle == 0) save(output + "/music.ppm");
        click("暂停并返回小智");
        assert(destination == AppTarget::Assistant && music.state == MusicService::State::Paused);
        assert(player.close());
        lv_obj_clean(screen);
        const auto shell = createLauncherAppShell(screen, "设置");
        buildSharedAudioSettings(shell.body);
        click("保存助手设置");
        assert(save_calls == static_cast<unsigned>(cycle + 1));
        lv_obj_clean(screen);
        advance();
        online = !online;
    }
    assert(start_calls == 100 && listen_calls == 100 && finish_calls == 100 && play_calls == 0);
    puts("PASS: production assistant/music views, BOOT events, pause button dispatch, shared settings and 100 cleanup cycles");
}
