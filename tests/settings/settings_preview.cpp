#include "launcher_apps.hpp"
#include "assistant_service.hpp"
#include "bluetooth_service.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
uint16_t buffer[360 * 360], pixels[360 * 360];
SystemService::Snapshot network;
AssistantService::Config config;
BluetoothService::Snapshot bluetooth;
std::string joined, password, forgotten;
bool accept = true;
int saves = 0, pairings = 0, scans = 0;
void advance() { lv_tick_inc(450); lv_timer_handler(); lv_obj_update_layout(lv_screen_active()); }
lv_obj_t *find(lv_obj_t *parent, const lv_obj_class_t *type, const char *text = nullptr)
{
    if (lv_obj_has_flag(parent, LV_OBJ_FLAG_HIDDEN)) return nullptr;
    if (lv_obj_check_type(parent, type) && (!text || (type == &lv_label_class && strcmp(lv_label_get_text(parent), text) == 0))) return parent;
    for (uint32_t i = 0; i < lv_obj_get_child_count(parent); ++i)
        if (auto *found = find(lv_obj_get_child(parent, i), type, text)) return found;
    return nullptr;
}
void click(const char *text)
{
    auto *label = find(lv_screen_active(), &lv_label_class, text);
    if (!label) { fprintf(stderr, "Missing: %s\n", text); assert(label); }
    lv_obj_send_event(lv_obj_get_parent(label), LV_EVENT_CLICKED, nullptr);
    advance();
}
void enter(const char *text)
{
    auto *input = find(lv_screen_active(), &lv_textarea_class);
    auto *keypad = find(lv_screen_active(), &lv_buttonmatrix_class);
    assert(input && keypad);
    lv_textarea_set_text(input, text);
    lv_obj_send_event(keypad, LV_EVENT_READY, nullptr);
    advance();
}
void save(const std::string &path)
{
    lv_obj_update_layout(lv_screen_active());
    lv_refr_now(nullptr);
    auto *file = fopen(path.c_str(), "wb"); assert(file);
    fprintf(file, "P6\n360 360\n255\n");
    for (auto value : pixels) {
        const unsigned char rgb[] = {static_cast<unsigned char>(((value >> 11) & 31) * 255 / 31), static_cast<unsigned char>(((value >> 5) & 63) * 255 / 63), static_cast<unsigned char>((value & 31) * 255 / 31)};
        fwrite(rgb, 1, 3, file);
    }
    fclose(file);
}
}
SystemService &SystemService::instance() { static SystemService s; return s; }
SystemService::Snapshot SystemService::snapshot() { return network; }
bool SystemService::scan() { ++scans; return accept; }
bool SystemService::connect(const char *ssid, const char *pass) { if (!accept) return false; joined = ssid; password = pass; return true; }
bool SystemService::forget() { forgotten = network.ssid; return accept; }
bool SystemService::saveBrightness(int v) { network.brightness = v; return accept; }
bool SystemService::testInternet() { return accept; }
bool SystemService::timeValid() { return false; }
const char *SystemService::stateText(NetworkState) { return "已连接"; }
AssistantService &AssistantService::instance() { static AssistantService s; return s; }
AssistantService::Config AssistantService::config() { return ::config; }
AssistantService::Snapshot AssistantService::snapshot() { return AssistantService::Snapshot(); }
bool AssistantService::saveConfig(const Config &c) { if (!accept) return false; ::config = c; ++saves; return true; }
bool AssistantService::setVolume(int v) { ::config.volume = v; return accept; }
BluetoothService &BluetoothService::instance() { static BluetoothService s; return s; }
BluetoothService::Snapshot BluetoothService::snapshot() { return bluetooth; }
bool BluetoothService::setEnabled(bool enabled) { bluetooth.enabled = enabled; return accept; }
bool BluetoothService::pairAgain() { ++pairings; return accept; }
int main(int argc, char **argv)
{
    assert(argc == 2);
    lv_init();
    auto *d = lv_display_create(360, 360);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, buffer, nullptr, sizeof(buffer), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(d, [](lv_display_t *display, const lv_area_t *a, uint8_t *data) {
        auto *src = reinterpret_cast<uint16_t *>(data);
        for (int y = a->y1; y <= a->y2; ++y) for (int x = a->x1; x <= a->x2; ++x) pixels[y * 360 + x] = *src++;
        lv_display_flush_ready(display);
    });
    network.ready = true; network.state = SystemService::NetworkState::Connected;
    strcpy(network.ssid, "Home"); strcpy(network.mac, "28:84:85:B2:1C:2C"); network.ap_count = 2;
    strcpy(network.aps[0].ssid, "Secure"); network.aps[0].secured = true; network.aps[0].rssi = -40;
    strcpy(network.aps[1].ssid, "Open"); network.aps[1].rssi = -60;
    bluetooth.ready = true; bluetooth.busy = false; bluetooth.battery_percent = 72;
    strcpy(bluetooth.address, "28:84:85:B2:1C:75");
    bluetooth.battery.valid = true; bluetooth.battery.percent = 72; bluetooth.battery.gaugePercent = 72;
    bluetooth.battery.voltageMv = 3900; bluetooth.battery.stale = false;
    const std::string out = argv[1];
    size_t baseline = 0;
    for (int cycle = 0; cycle < 30; ++cycle) {
        auto *old = lv_screen_active();
        auto *screen = lv_obj_create(nullptr); lv_screen_load(screen); lv_obj_delete(old);
        LauncherApp app(LauncherApp::Kind::Settings); assert(app.run()); advance();
        if (!cycle) save(out + "/settings-home.ppm");
        click("Wi-Fi");
        const auto scans_before = scans;
        click("扫描 Wi-Fi"); assert(scans == scans_before + 1);
        network.connection_pending = true; advance();
        assert(lv_obj_has_state(lv_obj_get_parent(find(screen, &lv_label_class, "扫描 Wi-Fi")), LV_STATE_DISABLED));
        network.connection_pending = false; advance();
        if (!cycle) save(out + "/settings-wifi.ppm");
        click("Secure");
        if (!cycle) save(out + "/settings-keypad.ppm");
        const auto previous = joined;
        enter(""); assert(find(screen, &lv_textarea_class) && joined == previous);
        enter("short"); assert(find(screen, &lv_textarea_class) && joined == previous);
        accept = false; enter("password123"); assert(find(screen, &lv_textarea_class));
        accept = true; enter("password123"); assert(joined == "Secure" && password == "password123");
        click("Open"); assert(joined == "Open" && password.empty());
        click("其他网络"); enter("Hidden"); enter(""); assert(joined == "Hidden" && password.empty());
        click("忘记当前网络"); ++network.scan_revision; advance();
        lv_tick_inc(4100); advance();
        assert(find(screen, &lv_label_class, "忘记当前网络"));
        app.back(); advance();
        click("声音与显示");
        if (!cycle) save(out + "/settings-sound.ppm");
        auto *slider = find(screen, &lv_slider_class); assert(slider);
        lv_slider_set_value(slider, 35, LV_ANIM_OFF);
        lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, nullptr); assert(config.volume != 35 || cycle > 0);
        lv_obj_send_event(slider, LV_EVENT_RELEASED, nullptr); assert(config.volume == 35);
        app.back(); advance();
        click("小智助手"); click("令牌"); enter("secret"); assert(std::string(config.token) == "secret");
        click("令牌"); app.back(); advance(); assert(std::string(config.token) == "secret");
        app.back(); advance();
        click("蓝牙");
        if (!cycle) save(out + "/settings-bluetooth.ppm");
        auto *sw = find(screen, &lv_switch_class); assert(sw);
        lv_obj_remove_state(sw, LV_STATE_CHECKED); lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, nullptr); assert(!bluetooth.enabled);
        lv_obj_add_state(sw, LV_STATE_CHECKED); lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, nullptr); assert(bluetooth.enabled);
        const int paired = pairings;
        click("重新配对"); lv_tick_inc(4100); advance(); assert(pairings == paired);
        click("重新配对"); click("再次点击清除配对"); assert(pairings == paired + 1);
        app.back(); advance();
        click("电池"); if (!cycle) save(out + "/settings-battery.ppm"); app.back(); advance();
        click("设备信息"); if (!cycle) save(out + "/settings-device.ppm"); app.back(); advance();
        const auto joins_before = joined;
        click("Wi-Fi"); click("Secure");
        lv_textarea_set_text(find(screen, &lv_textarea_class), "discarded-password");
        app.back(); advance(); assert(joined == joins_before);
        assert(!find(screen, &lv_textarea_class));
        click("Secure");
        auto *input = find(screen, &lv_textarea_class); assert(input && !lv_textarea_get_text(input)[0]);
        app.back(); advance(); assert(joined == joins_before);
        app.back(); advance();
        app.close();
        // Brookesia owns timers created by run; emulate that lifecycle here.
        auto *timer = lv_timer_get_next(nullptr);
        while (timer) { auto *next = lv_timer_get_next(timer); if (lv_timer_get_user_data(timer) == &app) lv_timer_delete(timer); timer = next; }
        auto *blank = lv_obj_create(nullptr); lv_screen_load(blank); lv_obj_delete(screen); advance();
        lv_mem_monitor_t mem{}; lv_mem_monitor(&mem);
        if (!cycle) baseline = mem.total_size - mem.free_size;
        else assert(mem.total_size - mem.free_size <= baseline + 128);

    }
    printf("PASS: production Muse settings UI, existing Wi-Fi/BLE/volume calls, secret editing, confirmation expiry and 30 lifecycle cycles\n");
}
