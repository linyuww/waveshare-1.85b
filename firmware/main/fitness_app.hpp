#pragma once
#include "esp_brookesia.hpp"
#include "fitness_storage.hpp"
#include "fitness_view.hpp"

class FitnessApp final : public esp_brookesia::systems::phone::App {
public:
    FitnessApp();
    bool run() override;
    bool back() override;
    bool close() override;
    bool pause() override;
    bool resume() override;
    bool cleanResource() override;
private:
    static void onTimer(lv_timer_t *timer);
    static void onHome(void *context);
    void tick();
    FitnessStorage storage_;
    fitness::Model model_;
    FitnessView view_;
    lv_timer_t *timer_ = nullptr;
    bool loaded_ = false;
};
