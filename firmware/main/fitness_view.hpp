#pragma once
#include "fitness_model.hpp"
#include "lvgl.h"
#include <array>
#include <string>

class FitnessView {
public:
    enum class Page { Home, Days, Training, Cues, History, Record, Summary, Settings, Goal, Templates, Plan, ConfirmEnd };
    FitnessView(fitness::Model &model, void (*home)(void *), void *context);
    void attach(lv_obj_t *root);
    void detach();
    void releaseMedia();
    void tick(uint64_t now, int weekday, int64_t synced_epoch);
    void show(Page page);
    void refresh();
    bool back();
    Page page() const { return page_; }
    void forceMediaFailure(bool value) { force_media_failure_ = value; dirty_ = true; }
private:
    enum class Action { Home, Back, Start, Day, Page, Count, Next, Undo, Pause, Skip, Finish,
        Record, Goal, SetsMinus, SetsPlus, RepsMinus, RepsPlus, Rest, Plan, Move, Remove, Add, Up, Retry };
    struct Binding { FitnessView *view; Action action; int argument; };
    static void onEvent(lv_event_t *event);
    void act(Action action, int argument);
    lv_obj_t *label(lv_obj_t *parent, const char *text, int width = 0);
    lv_obj_t *button(lv_obj_t *parent, const char *text, Action action, int argument = 0, int width = 0);
    lv_obj_t *row(lv_obj_t *parent);
    lv_obj_t *frame(const char *title, bool scroll = true);
    void homePage();
    void trainingPage();
    void historyPage();
    void recordPage(const fitness::Record *record);
    void settingsPage();
    void goalPage();
    void planPage();
    static std::string date(const fitness::Record &record);
    fitness::Model &model_;
    void (*home_)(void *);
    void *context_;
    lv_obj_t *root_ = nullptr;
    lv_obj_t *media_ = nullptr;
    lv_obj_t *countdown_ = nullptr;
    std::array<Binding, 96> bindings_{};
    size_t binding_count_ = 0;
    Page page_ = Page::Home;
    int selected_day_ = -1;
    int exercise_ = 0;
    int plan_day_ = 0;
    int record_ = 0;
    int weekday_ = 0;
    int64_t epoch_ = 0;
    uint64_t now_ = 0;
    bool dirty_ = true;
    bool manual_day_ = false;
    bool force_media_failure_ = false;
};
