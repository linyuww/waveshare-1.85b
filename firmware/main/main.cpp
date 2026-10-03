#include "bsp/esp-bsp.h"
#include "esp_brookesia.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "launcher_apps.hpp"
#include "bluetooth_service.hpp"
#include "codex_micro_app.hpp"
#include "system_service.hpp"
#include "fitness_app.hpp"
#include "audio_apps.hpp"
#include "app_navigation.hpp"
#include "assistant_service.hpp"
#include "music_service.hpp"
#include "shared_audio.hpp"
#include "ui_assets.h"
#include "dark/stylesheet.hpp"
#include <ctime>

using namespace esp_brookesia::systems::phone;
using esp_brookesia::gui::LvLock;
using esp_brookesia::gui::LvLockGuard;

namespace {
constexpr char TAG[] = "launcher";
Phone *phone;
lv_obj_t *splash;
lv_timer_t *status_timer;
bool brightness_restored = false;
int app_ids[4] = {-1, -1, -1, -1};

void navigateApps(lv_timer_t *)
{
    AppTarget target;
    if (AppNavigation::poll(target)) {
        const int id = app_ids[static_cast<int>(target)];
        const esp_brookesia::systems::base::Context::AppEventData event = {
            id, esp_brookesia::systems::base::Context::AppEventType::START, nullptr
        };
        if (id >= esp_brookesia::systems::base::App::APP_ID_MIN && !phone->sendAppEvent(&event)) {
            ESP_LOGW(TAG, "Requested application could not start");
        }
    }
}

void updateStatus(lv_timer_t *)
{
    static unsigned updates = 0;
    if (++updates % 60 == 0) {
        ESP_LOGI(TAG, "Heap: internal=%u DMA-largest=%u PSRAM=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    }
    auto status = SystemService::instance().snapshot();
    auto *bar = phone->getDisplay().getStatusBar();
    const int wifi_state = status.state == SystemService::NetworkState::Connected ?
        (status.rssi >= -60 ? 3 : status.rssi >= -75 ? 2 : 1) : 0;
    bar->setWifiIconState(wifi_state);
    if (SystemService::timeValid()) {
        const time_t now = time(nullptr);
        tm local = {};
        localtime_r(&now, &local);
        bar->setClock(local.tm_hour, local.tm_min);
    }
    if (status.ready && !brightness_restored) {
        const esp_err_t err = bsp_display_brightness_set(status.brightness);
        brightness_restored = err == ESP_OK;
        if (err != ESP_OK) ESP_LOGW(TAG, "Brightness restore: %s", esp_err_to_name(err));
    }
}

void startDesktop(lv_timer_t *timer)
{
    // This callback runs under the LVGL adapter lock.
    if (!phone->begin()) {
        ESP_LOGE(TAG, "Desktop initialization failed");
        lv_timer_delete(timer);
        return;
    }
    for (auto kind : {LauncherApp::Kind::Settings, LauncherApp::Kind::Clock, LauncherApp::Kind::Network, LauncherApp::Kind::About}) {
        auto *app = new LauncherApp(kind);
        const int id = phone->installApp(app);
        if (kind == LauncherApp::Kind::Settings) app_ids[static_cast<int>(AppTarget::Settings)] = id;
        if (id < esp_brookesia::systems::base::App::APP_ID_MIN) {
            ESP_LOGE(TAG, "App installation failed");
            delete app;
        }
    }
    auto *codex_app = new CodexMicroApp();
    app_ids[static_cast<int>(AppTarget::Codex)] = phone->installApp(codex_app);
    if (app_ids[static_cast<int>(AppTarget::Codex)] < esp_brookesia::systems::base::App::APP_ID_MIN) {
        ESP_LOGE(TAG, "Codex Micro installation failed");
        delete codex_app;
    }
    auto *fitness_app = new FitnessApp();
    if (phone->installApp(fitness_app) < esp_brookesia::systems::base::App::APP_ID_MIN) {
        ESP_LOGE(TAG, "Fitness installation failed");
        delete fitness_app;
    }
    for (auto kind : {AudioApp::Kind::Assistant, AudioApp::Kind::Music}) {
        auto *app = new AudioApp(kind);
        const int id = phone->installApp(app);
        app_ids[static_cast<int>(kind == AudioApp::Kind::Assistant ? AppTarget::Assistant : AppTarget::Music)] = id;
        if (id < esp_brookesia::systems::base::App::APP_ID_MIN) {
            ESP_LOGE(TAG, "Audio app installation failed");
            delete app;
        }
    }
    lv_timer_create(navigateApps, 50, nullptr);
    // Hide the unimplemented battery gauge instead of displaying an invented charge level.
    phone->getDisplay().getStatusBar()->hideBatteryIcon();
    phone->getDisplay().getStatusBar()->setClockFormat(StatusBar::ClockFormat::FORMAT_24H);
    status_timer = lv_timer_create(updateStatus, 1000, nullptr);
    updateStatus(status_timer);
    if (splash && splash != lv_screen_active()) lv_obj_delete(splash);
    splash = nullptr;
    lv_timer_delete(timer);
}
}

extern "C" void app_main()
{
    heap_caps_malloc_extmem_enable(0);
    ESP_ERROR_CHECK(SystemService::instance().start());
    bsp_display_cfg_t display_config = {};
    display_config.lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    display_config.lv_adapter_cfg.task_stack_size = 16384;
    display_config.lv_adapter_cfg.task_core_id = 1;
    display_config.rotation = ESP_LV_ADAPTER_ROTATE_0;
    display_config.tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE;
    auto *display = bsp_display_start_with_config(&display_config);
    if (!display) { ESP_LOGE(TAG, "Display initialization failed"); return; }
    auto *touch = bsp_display_get_input_dev();
    const esp_err_t audio_result = shared_audio::init();
    if (audio_result != ESP_OK) ESP_LOGW(TAG, "Shared audio unavailable: %s", esp_err_to_name(audio_result));
    ESP_ERROR_CHECK(BluetoothService::instance().start());
    ESP_ERROR_CHECK(AppNavigation::initialize() ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(AssistantService::instance().initialize());
    ESP_ERROR_CHECK(MusicService::instance().initialize());
    LvLock::registerCallbacks([](int timeout) { return bsp_display_lock(timeout) == ESP_OK; }, []() { bsp_display_unlock(); return true; });
    LvLockGuard guard;
    splash = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(splash, lv_color_hex(0x101923), 0);
    auto *brand = lv_label_create(splash);
    lv_obj_set_style_text_font(brand, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(brand, lv_color_hex(0x68D8C6), 0);
    lv_label_set_text(brand, "WAVE");
    lv_obj_align(brand, LV_ALIGN_CENTER, 0, -30);
    auto *hint = lv_label_create(splash);
    lv_obj_set_style_text_font(hint, &ui_font, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0xAAB9CB), 0);
    lv_label_set_text(hint, "正在启动应用桌面");
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 35);
    lv_screen_load(splash);
    ESP_ERROR_CHECK(bsp_display_backlight_on());
    phone = new Phone(display);
    phone->setTouchDevice(touch);
    static Stylesheet stylesheet(STYLESHEET_360_360_DARK);
    if (!phone->addStylesheet(&stylesheet) || !phone->activateStylesheet(&stylesheet)) {
        ESP_LOGE(TAG, "Stylesheet initialization failed");
        lv_label_set_text(hint, "桌面样式加载失败");
        return;
    }
    lv_timer_create(startDesktop, 1200, nullptr);
}
