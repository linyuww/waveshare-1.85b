#pragma once
#include "esp_brookesia.hpp"
#include "assistant_service.hpp"

class AudioApp : public esp_brookesia::systems::phone::App {
public:
    enum class Kind { Assistant, Music };
    explicit AudioApp(Kind kind);
    bool run() override;
    bool back() override;
    bool close() override;
private:
    static void onAction(lv_event_t *event);
    static void onFocus(lv_event_t *event);
    static void onKeyboard(lv_event_t *event);
    static void onTimer(lv_timer_t *timer);
    lv_obj_t *button(lv_obj_t *parent, const char *text, int action);
    void refresh();
    Kind kind_;
    lv_obj_t *root_ = nullptr;
    lv_obj_t *status_ = nullptr;
    lv_obj_t *transcript_ = nullptr;
    lv_obj_t *song_ = nullptr;
    lv_obj_t *artist_ = nullptr;
    lv_obj_t *keyboard_ = nullptr;
    lv_timer_t *timer_ = nullptr;
    bool boot_pressed_ = false;
};

void buildSharedAudioSettings(lv_obj_t *body);
