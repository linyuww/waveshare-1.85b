#pragma once
#include "esp_brookesia.hpp"
#include "assistant_service.hpp"
#include "boot_button.hpp"

class AudioApp : public esp_brookesia::systems::phone::App {
public:
    AudioApp();
    bool run() override;
    bool back() override;
    bool close() override;
    bool pause() override;
    bool resume() override;
private:
    static void onHome(lv_event_t *event);
    static void onToggle(lv_event_t *event);
    static void onTimer(lv_timer_t *timer);
    static void onRootDeleted(lv_event_t *event);
    void refresh();
    lv_obj_t *root_ = nullptr;
    lv_obj_t *status_ = nullptr;
    lv_obj_t *transcript_ = nullptr;
    lv_obj_t *conversation_ = nullptr;
    lv_obj_t *speaker_ = nullptr;
    lv_obj_t *activation_ = nullptr;
    lv_obj_t *hint_ = nullptr;
    lv_timer_t *timer_ = nullptr;
    bool active_ = false;
    BootButton boot_;
    uint32_t boot_session_ = 0;
};

void buildSharedAudioSettings(lv_obj_t *body);
