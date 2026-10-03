#pragma once
#include "esp_brookesia.hpp"
#include "system_service.hpp"

class LauncherApp : public esp_brookesia::systems::phone::App {
public:
    enum class Kind { Settings, Clock, Network, About };
    explicit LauncherApp(Kind kind);
    bool run() override;
    bool back() override;
    bool close() override;

private:
    static void onAction(lv_event_t *event);
    static void onNetworkSelected(lv_event_t *event);
    static void onTextFocused(lv_event_t *event);
    static void onKeyboard(lv_event_t *event);
    static void onBrightness(lv_event_t *event);
    static void onTimer(lv_timer_t *timer);
    void refresh();
    void buildSettings(lv_obj_t *body);
    void buildClock(lv_obj_t *body);
    void buildNetwork(lv_obj_t *body);
    void buildAbout(lv_obj_t *body);
    void showConnectionForm(const char *ssid, bool secured);
    void hideConnectionForm();
    void submitConnection();
    void updateScanList(const SystemService::Snapshot &status);

    Kind kind_;
    lv_obj_t *root_ = nullptr;
    lv_obj_t *message_ = nullptr;
    lv_obj_t *network_list_ = nullptr;
    lv_obj_t *clock_ = nullptr;
    lv_obj_t *date_ = nullptr;
    lv_obj_t *result_ = nullptr;
    lv_obj_t *battery_label_ = nullptr;
    lv_obj_t *scan_button_ = nullptr;
    lv_obj_t *test_button_ = nullptr;
    lv_obj_t *brightness_label_ = nullptr;
    lv_obj_t *bluetooth_label_ = nullptr;
    lv_obj_t *bluetooth_switch_ = nullptr;
    lv_obj_t *pair_button_ = nullptr;
    lv_obj_t *form_ = nullptr;
    lv_obj_t *ssid_input_ = nullptr;
    lv_obj_t *password_input_ = nullptr;
    lv_obj_t *keyboard_ = nullptr;
    uint32_t scan_revision_ = UINT32_MAX;
    SystemService::AccessPoint listed_aps_[SystemService::MAX_APS] = {};
    int listed_ap_count_ = 0;
    bool secured_ = true;
};
