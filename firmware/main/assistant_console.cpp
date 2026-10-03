#include "assistant_console.hpp"
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include "app_navigation.hpp"
#include "assistant_service.hpp"
#include "music_service.hpp"
#include "system_service.hpp"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

namespace assistant_console {
namespace {
bool ready;
char line[320];
size_t used;
bool overflow;
int command(int count, char **arguments)
{
    if (count != 2) {
        puts("Usage: xiaozhi status|connect|stop|open");
        return 1;
    }
    auto &assistant = AssistantService::instance();
    if (strcmp(arguments[1], "status") == 0) {
        const auto state = assistant.snapshot();
        const auto network = SystemService::instance().snapshot();
        printf("xiaozhi: wifi=%s time_valid=%d connected=%d listening=%d speaking=%d\n",
            SystemService::stateText(network.state), SystemService::timeValid(), state.connected, state.listening, state.speaking);
        printf("xiaozhi: message=%s activation=%s internal=%u psram=%u\n", state.message,
            state.activation[0] ? state.activation : "none", static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        return 0;
    }
    const bool result = strcmp(arguments[1], "connect") == 0 ? assistant.start() :
        strcmp(arguments[1], "stop") == 0 ? assistant.stop() :
        strcmp(arguments[1], "open") == 0 ? AppNavigation::request(AppTarget::Assistant) : false;
    printf("xiaozhi: command %s\n", result ? "accepted" : "rejected");
    return result ? 0 : 1;
}

int musicCommand(int count, char **arguments)
{
    auto &music = MusicService::instance();
    if (count == 2 && strcmp(arguments[1], "status") == 0) {
        const auto state = music.snapshot();
        printf("music: state=%d playback_ms=%u song=%s artist=%s message=%s\n",
            static_cast<int>(state.state), static_cast<unsigned>(state.playback_ms),
            state.song, state.artist, state.message);
        return 0;
    }
    if (count == 3 && strcmp(arguments[1], "config") == 0) {
        auto config = AssistantService::instance().config();
        if (strlen(arguments[2]) >= sizeof(config.music_url)) return 1;
        strlcpy(config.music_url, arguments[2], sizeof(config.music_url));
        const bool accepted = AssistantService::instance().saveConfig(config);
        printf("music: configuration %s\n", accepted ? "queued" : "rejected");
        return accepted ? 0 : 1;
    }
    if ((count == 3 || count == 4) && strcmp(arguments[1], "play") == 0) {
        const bool accepted = music.play(arguments[2], count == 4 ? arguments[3] : "");
        printf("music: play %s\n", accepted ? "accepted" : "rejected");
        return accepted ? 0 : 1;
    }
    if (count == 2 && strcmp(arguments[1], "pause") == 0) { music.pause(); return 0; }
    if (count == 2 && strcmp(arguments[1], "stop") == 0) { music.stop(); return 0; }
    if (count == 2 && strcmp(arguments[1], "resume") == 0) return music.resume() ? 0 : 1;
    puts("Usage: music status|config <base-url>|play <song> [artist]|pause|resume|stop");
    return 1;
}
}

esp_err_t initialize()
{
    usb_serial_jtag_vfs_use_nonblocking();
    const int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK) < 0) return ESP_FAIL;
    esp_console_config_t config = ESP_CONSOLE_CONFIG_DEFAULT();
    config.max_cmdline_args = 4;
    config.max_cmdline_length = sizeof(line);
    config.heap_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    esp_err_t result = esp_console_init(&config);
    if (result != ESP_OK) return result;
    esp_console_cmd_t entry = {};
    entry.command = "xiaozhi";
    entry.help = "Inspect assistant status, start/stop the connection or open its page";
    entry.func = command;
    result = esp_console_cmd_register(&entry);
    if (result == ESP_OK) {
        entry.command = "music";
        entry.help = "Configure the PCM music bridge or control music playback";
        entry.func = musicCommand;
        result = esp_console_cmd_register(&entry);
    }
    if (result == ESP_OK) result = esp_console_register_help_command();
    ready = result == ESP_OK;
    if (ready) ESP_LOGI("xiaozhi", "USB diagnostics ready: xiaozhi status|connect|stop|open, help");
    return result;
}

void poll()
{
    if (!ready) return;
    char incoming[32];
    int length = 0;
    while (length < static_cast<int>(sizeof(incoming)) && read(STDIN_FILENO, incoming + length, 1) == 1) ++length;
    for (int index = 0; index < length; ++index) {
        const char byte = incoming[index];
        if (byte == '\r' || byte == '\n') {
            if (overflow) puts("xiaozhi: command too long");
            else if (used) {
                line[used] = 0;
                int returned = 0;
                if (esp_console_run(line, &returned) != ESP_OK) puts("xiaozhi: unknown or invalid command");
            }
            used = 0;
            overflow = false;
        } else if (byte == '\b' || byte == 127) {
            if (used && !overflow) --used;
        } else if (static_cast<unsigned char>(byte) >= 32 && byte != 127 && !overflow) {
            if (used + 1 < sizeof(line)) line[used++] = byte;
            else overflow = true;
        }
    }
}
}
