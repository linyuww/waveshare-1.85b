#pragma once
#include "esp_brookesia.hpp"
#include "gfx.h"
#include "logic.h"
#include "codex_boot_control.hpp"

class CodexMicroApp : public esp_brookesia::systems::phone::App {
public:
    CodexMicroApp();
    bool run() override;
    bool back() override;
    bool close() override;
    bool pause() override;
    bool resume() override;
    bool cleanResource() override;

private:
    static void onTimer(lv_timer_t *timer);
    static void onTouch(lv_event_t *event);
    static void onHome(lv_event_t *event);
    void tick();
    void render();
    void releaseControls();
    void restoreBrightness();
    bool wake();
    void press(int x, int y);
    void move(int x, int y);
    void release();
    void updateButton();

    gfx::Canvas canvas_;
    lv_image_dsc_t frame_ = {};
    lv_obj_t *root_ = nullptr;
    lv_obj_t *image_ = nullptr;
    lv_timer_t *timer_ = nullptr;
    bool active_ = false;
    bool night_ = false;
    bool sleeping_ = false;
    bool consume_touch_ = false;
    bool tracking_ = false;
    bool send_ = false;
    bool power_hold_ = false;
    CodexBootControl boot_;
    int agent_ = -1;
    int start_x_ = 0;
    int start_y_ = 0;
    int brightness_ = 70;
    touch_gesture::Direction direction_ = touch_gesture::Direction::None;
    uint32_t touch_at_ = 0;
    uint32_t voice_until_ = 0;
    uint32_t activity_at_ = 0;
    uint32_t rendered_at_ = 0;
    uint32_t revision_ = UINT32_MAX;
};
