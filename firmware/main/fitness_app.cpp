#include "fitness_app.hpp"
#include "esp_timer.h"
#include "system_service.hpp"
#include "ui_assets.h"
#include <ctime>

namespace {
uint64_t nowMs() { return static_cast<uint64_t>(esp_timer_get_time() / 1000); }
}
FitnessApp::FitnessApp() : App("健身", &icon_fitness, true, false, false),
    model_(storage_), view_(model_, onHome, this) {}
bool FitnessApp::run()
{
    if (!loaded_) { model_.load(); loaded_ = true; }
    view_.show(FitnessView::Page::Home);
    view_.attach(lv_screen_active());
    timer_ = lv_timer_create(onTimer, 100, this);
    tick();
    return true;
}
bool FitnessApp::back() { return view_.back(); }
bool FitnessApp::pause()
{
    if (model_.state().session.active || model_.saveFailed()) model_.checkpoint(nowMs());
    view_.releaseMedia();
    if (timer_) lv_timer_pause(timer_);
    return true;
}
bool FitnessApp::resume()
{
    view_.show(view_.page());
    if (timer_) lv_timer_resume(timer_);
    tick();
    return true;
}
bool FitnessApp::close()
{
    pause();
    view_.detach();
    if (timer_) { lv_timer_delete(timer_); timer_ = nullptr; }
    return true;
}
bool FitnessApp::cleanResource() { return true; }
void FitnessApp::onHome(void *context) { static_cast<FitnessApp *>(context)->notifyCoreClosed(); }
void FitnessApp::onTimer(lv_timer_t *timer) { static_cast<FitnessApp *>(lv_timer_get_user_data(timer))->tick(); }
void FitnessApp::tick()
{
    const bool valid = SystemService::timeValid();
    const auto epoch = valid ? time(nullptr) : 0;
    tm local{};
    if (valid) localtime_r(&epoch, &local);
    view_.tick(nowMs(), local.tm_wday, epoch);
}
