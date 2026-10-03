#include "fitness_view.hpp"
#include "fitness_assets.h"
#include "ui_assets.h"
#include <algorithm>
#include <cstdio>
#include <ctime>

namespace {
constexpr uint32_t background = 0x0C1A30;
constexpr uint32_t accent = 0x5BB8FF;
}
FitnessView::FitnessView(fitness::Model &model, void (*home)(void *), void *context)
    : model_(model), home_(home), context_(context) {}
void FitnessView::attach(lv_obj_t *root) { root_ = root; dirty_ = true; refresh(); }
void FitnessView::releaseMedia() { if (media_) { lv_obj_delete(media_); media_ = nullptr; } }
void FitnessView::detach() { releaseMedia(); root_ = countdown_ = nullptr; }
void FitnessView::show(Page page) { page_ = page; dirty_ = true; }
void FitnessView::tick(uint64_t now, int weekday, int64_t synced_epoch)
{
    now_ = now;
    if (epoch_ == 0 && synced_epoch != 0 && page_ == Page::Home) dirty_ = true;
    if (weekday_ != weekday && page_ == Page::Home) dirty_ = true;
    weekday_ = weekday;
    epoch_ = synced_epoch;
    if (model_.tick(now)) dirty_ = true;
    refresh();
    if (countdown_) {
        const auto seconds = (model_.remaining(now) + 999) / 1000;
        lv_label_set_text_fmt(countdown_, "%02lu:%02lu", static_cast<unsigned long>(seconds / 60), static_cast<unsigned long>(seconds % 60));
    }
}
lv_obj_t *FitnessView::label(lv_obj_t *parent, const char *text, int width)
{
    auto *object = lv_label_create(parent);
    lv_obj_set_style_text_font(object, &ui_font, 0);
    lv_obj_set_style_text_color(object, lv_color_hex(0xEEF6FF), 0);
    if (width) lv_obj_set_width(object, width);
    lv_label_set_text(object, text);
    return object;
}
lv_obj_t *FitnessView::button(lv_obj_t *parent, const char *text, Action action, int argument, int width)
{
    auto *object = lv_button_create(parent);
    lv_obj_set_size(object, width ? width : 244, 42);
    lv_obj_set_style_bg_color(object, lv_color_hex(0x1E3752), 0);
    lv_obj_set_style_shadow_width(object, 0, 0);
    lv_obj_set_style_radius(object, 12, 0);
    lv_obj_set_style_pad_all(object, 3, 0);
    auto *caption = label(object, text);
    lv_obj_center(caption);
    if (binding_count_ < bindings_.size()) {
        auto &binding = bindings_[binding_count_++];
        binding = {this, action, argument};
        lv_obj_add_event_cb(object, onEvent, LV_EVENT_CLICKED, &binding);
    }
    return object;
}
lv_obj_t *FitnessView::row(lv_obj_t *parent)
{
    auto *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_size(object, 244, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(object, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(object, 8, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    return object;
}
lv_obj_t *FitnessView::frame(const char *title, bool scroll)
{
    lv_obj_set_style_bg_color(root_, lv_color_hex(background), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_radius(root_, 0, 0);
    lv_obj_set_layout(root_, LV_LAYOUT_NONE);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    auto *heading = label(root_, title, 200);
    lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(heading, lv_color_hex(accent), 0);
    lv_obj_set_pos(heading, 80, 24);
    auto *body = lv_obj_create(root_);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, 264, scroll ? 222 : 236);
    lv_obj_set_pos(body, 48, 55);
    lv_obj_set_style_pad_all(body, 10, 0);
    lv_obj_set_style_pad_row(body, 8, 0);
    lv_obj_set_style_bg_color(body, lv_color_hex(background), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(body, 16, 0);
    lv_obj_set_style_clip_corner(body, true, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
    if (scroll) {
        lv_obj_set_scroll_dir(body, LV_DIR_VER);
        auto *hint = label(root_, "上滑查看更多", 160);
        lv_obj_set_pos(hint, 100, 280);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(hint, lv_color_hex(0x93CCF6), 0);
    } else lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    auto *controls = row(root_);
    lv_obj_set_size(controls, 136, 42);
    lv_obj_set_pos(controls, 112, 302);
    button(controls, "桌面", Action::Home, 0, 64);
    button(controls, "返回", Action::Back, 0, 64);
    if (model_.saveFailed()) {
        auto *warning = button(root_, "保存失败 · 点此重试", Action::Retry, 0, 220);
        lv_obj_set_pos(warning, 70, 58);
        lv_obj_set_style_bg_color(warning, lv_color_hex(0x754028), 0);
        lv_obj_set_height(body, scroll ? 175 : 186);
        lv_obj_set_y(body, 105);
    }
    return body;
}
std::string FitnessView::date(const fitness::Record &record)
{
    if (!record.started) return "时间未同步";
    const time_t epoch = static_cast<time_t>(record.started);
    tm local{};
    localtime_r(&epoch, &local);
    char text[40];
    strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local);
    return text;
}
void FitnessView::homePage()
{
    auto *body = frame("健身 · 记组");
    const bool active = model_.state().session.active;
    const int today = fitness::Model::today(weekday_, epoch_ != 0);
    label(body, epoch_ == 0 ? "时间未同步 · 手动选日" : today < 0 ? "今日休息 · 也可自由训练" : fitness::day_names[today], 244);
    if (active) {
        label(body, "上次训练尚未结束", 244);
        button(body, "继续训练", Action::Start);
    } else {
        if (!manual_day_) selected_day_ = today;
        if (selected_day_ >= 0) {
            label(body, fitness::day_names[selected_day_], 244);
            for (const auto move : model_.state().settings.plans[selected_day_].moves) {
                if (move == fitness::no_exercise) continue;
                const auto goal = model_.state().settings.goals[move];
                char text[96];
                snprintf(text, sizeof(text), "%s  %u组 × 目标%u次", fitness::exercises[move].name, goal.sets, goal.reps);
                label(body, text, 244);
            }
            button(body, "开始训练", Action::Start);
        }
    }
    button(body, "选择其他训练日", Action::Page, static_cast<int>(Page::Days));
    auto *controls = row(body);
    button(controls, "历史", Action::Page, static_cast<int>(Page::History), 118);
    button(controls, "设置", Action::Page, static_cast<int>(Page::Settings), 118);
}
void FitnessView::trainingPage()
{
    const auto &session = model_.state().session;
    if (!session.active) { page_ = Page::Summary; recordPage(model_.history(0)); return; }
    const auto exercise = session.record.moves[session.index];
    const auto goal = session.record.goals[session.index];
    const bool resting = session.rest != fitness::Rest::None;
    auto *body = frame(resting ? "组间休息" : fitness::exercises[exercise].name, resting || model_.saveFailed());
    if (!resting && !model_.saveFailed()) {
        lv_obj_set_y(body, 53);
        lv_obj_set_height(body, 245);
        lv_obj_set_style_pad_top(body, 0, 0);
        lv_obj_set_style_pad_bottom(body, 0, 0);
        lv_obj_set_style_pad_row(body, 4, 0);
    }
    if (resting) {
        label(body, fitness::exercises[exercise].name, 244);
        countdown_ = label(body, "01:30", 244);
        lv_obj_set_style_text_font(countdown_, &lv_font_montserrat_48, 0);
        lv_obj_set_style_text_align(countdown_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(countdown_, lv_color_hex(accent), 0);
        label(body, session.rest == fitness::Rest::Paused ? "已暂停 · 点继续开始计时" : "休息期间不能重复记组", 244);
        auto *controls = row(body);
        button(controls, session.rest == fitness::Rest::Paused ? "继续" : "暂停", Action::Pause, 0, 118);
        button(controls, "跳过", Action::Skip, 0, 118);
        button(body, "撤销上一组", Action::Undo);
        button(body, "提前结束", Action::Page, static_cast<int>(Page::ConfirmEnd));
        return;
    }
    auto *media_box = lv_obj_create(body);
    lv_obj_remove_style_all(media_box);
    lv_obj_set_size(media_box, 244, 120);
    lv_obj_remove_flag(media_box, LV_OBJ_FLAG_SCROLLABLE);
    bool loaded = false;
    if (!force_media_failure_) {
        media_ = lv_gif_create(media_box);
        lv_gif_set_color_format(media_, LV_COLOR_FORMAT_RGB565);
        lv_gif_set_src(media_, &fitness_gifs[exercise]);
        loaded = lv_gif_is_loaded(media_);
        if (loaded) lv_obj_center(media_);
        else releaseMedia();
    }
    if (!loaded) {
        media_ = lv_image_create(media_box);
        lv_image_set_src(media_, &fitness_stills[exercise]);
        lv_image_set_scale(media_, 512);
        lv_obj_center(media_);
        auto *hint = label(media_box, "动图不可用 · 可继续记组", 244);
        lv_obj_set_style_text_color(hint, lv_color_hex(accent), 0);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
    char text[72];
    snprintf(text, sizeof(text), "完成 %u/%u 组 · 目标%u次", session.record.done[session.index], goal.sets, goal.reps);
    label(body, text, 244);
    const bool done = session.record.done[session.index] == goal.sets;
    auto *primary = button(body, done ? (session.index + 1 == session.record.count ? "训练总结" : "下一动作") : "完成一组", done ? Action::Next : Action::Count);
    lv_obj_set_style_bg_color(primary, lv_color_hex(0x2469A2), 0);
    auto *controls = row(body);
    button(controls, "要点", Action::Page, static_cast<int>(Page::Cues), 76);
    auto *undo = button(controls, "撤销", Action::Undo, 0, 76);
    if (session.undo_index < 0) lv_obj_add_state(undo, LV_STATE_DISABLED);
    button(controls, "结束", Action::Page, static_cast<int>(Page::ConfirmEnd), 76);
}
void FitnessView::historyPage()
{
    auto *body = frame("最近 30 次训练");
    if (!model_.state().history_size) label(body, "还没有训练记录", 244);
    for (size_t index = 0; index < model_.state().history_size; ++index) {
        const auto *record = model_.history(index);
        label(body, date(*record).c_str(), 244);
        char text[80];
        snprintf(text, sizeof(text), "%s · %s", fitness::day_names[record->day], record->complete ? "完成" : "部分完成");
        button(body, text, Action::Record, static_cast<int>(index));
    }
}
void FitnessView::recordPage(const fitness::Record *record)
{
    auto *body = frame(page_ == Page::Summary ? "训练总结" : "训练详情");
    if (!record) { label(body, "还没有训练记录", 244); return; }
    label(body, date(*record).c_str(), 244);
    label(body, fitness::day_names[record->day], 244);
    label(body, record->complete ? "本次计划已完成" : "提前结束 · 已保存部分进度", 244);
    for (size_t index = 0; index < record->count; ++index) {
        char text[128];
        snprintf(text, sizeof(text), "%s\n完成 %u/%u 组 · 目标 %u 次\n%s", fitness::exercises[record->moves[index]].name,
            record->done[index], record->goals[index].sets, record->goals[index].reps,
            record->done[index] == record->goals[index].sets ? "已完成" : "未完成");
        label(body, text, 244);
    }
    label(body, "次数为目标值，不是实际计数", 244);
    button(body, "回到健身首页", Action::Page, static_cast<int>(Page::Home));
}
void FitnessView::settingsPage()
{
    auto *body = frame("健身设置");
    label(body, "调整仅用于新训练", 244);
    for (size_t index = 0; index < fitness::rest_options.size(); ++index) {
        char text[64];
        const auto seconds = fitness::rest_options[index];
        snprintf(text, sizeof(text), "组间休息 %u 秒%s", seconds, seconds == model_.state().settings.rest_seconds ? " · 当前" : "");
        button(body, text, Action::Rest, seconds);
    }
    button(body, "调整周计划模板", Action::Page, static_cast<int>(Page::Templates));
    for (size_t index = 0; index < fitness::exercise_count; ++index)
        button(body, fitness::exercises[index].name, Action::Goal, static_cast<int>(index));
}
void FitnessView::goalPage()
{
    auto *body = frame("动作目标");
    label(body, fitness::exercises[exercise_].name, 244);
    const auto goal = model_.state().settings.goals[exercise_];
    char text[80];
    snprintf(text, sizeof(text), "%u 组 × 目标 %u 次", goal.sets, goal.reps);
    label(body, text, 244);
    auto *sets = row(body);
    button(sets, "组数 -", Action::SetsMinus, 0, 118);
    button(sets, "组数 +", Action::SetsPlus, 0, 118);
    auto *reps = row(body);
    button(reps, "次数 -", Action::RepsMinus, 0, 118);
    button(reps, "次数 +", Action::RepsPlus, 0, 118);
    label(body, "1–10 组 · 1–50 次\n每次调整立即保存\n不影响正在进行的训练", 244);
}
void FitnessView::planPage()
{
    auto *body = frame("周计划模板");
    label(body, fitness::day_names[plan_day_], 244);
    const auto &moves = model_.state().settings.plans[plan_day_].moves;
    for (size_t index = 0; index < moves.size(); ++index) {
        if (moves[index] == fitness::no_exercise) continue;
        label(body, fitness::exercises[moves[index]].name, 244);
        auto *controls = row(body);
        button(controls, "更换", Action::Move, static_cast<int>(index), 76);
        button(controls, "上移", Action::Up, static_cast<int>(index), 76);
        button(controls, "移除", Action::Remove, static_cast<int>(index), 76);
    }
    button(body, "添加动作", Action::Add);
    label(body, "更换循环选择未重复动作\n每日至少 1 个、最多 4 个动作\n调整仅用于新训练", 244);
}
void FitnessView::refresh()
{
    if (!root_ || !dirty_) return;
    releaseMedia();
    countdown_ = nullptr;
    lv_obj_clean(root_);
    binding_count_ = 0;
    dirty_ = false;
    switch (page_) {
    case Page::Home: homePage(); break;
    case Page::Days: {
        auto *body = frame("选择训练日");
        if (model_.state().session.active) label(body, "请先结束当前训练", 244);
        for (size_t index = 0; index < fitness::day_count; ++index)
            button(body, fitness::day_names[index], Action::Day, static_cast<int>(index));
        break;
    }
    case Page::Training: trainingPage(); break;
    case Page::Cues: {
        auto *body = frame("动作要点");
        const auto &session = model_.state().session;
        const auto move = session.record.moves[session.index];
        label(body, fitness::exercises[move].name, 244);
        label(body, fitness::exercises[move].cue, 244);
        label(body, "简化示意，不作为个体训练指导", 244);
        button(body, "返回训练", Action::Page, static_cast<int>(Page::Training));
        break;
    }
    case Page::History: historyPage(); break;
    case Page::Record: recordPage(model_.history(record_)); break;
    case Page::Summary: recordPage(model_.history(0)); break;
    case Page::Settings: settingsPage(); break;
    case Page::Goal: goalPage(); break;
    case Page::Templates: {
        auto *body = frame("选择计划模板");
        for (size_t index = 0; index < fitness::day_count; ++index)
            button(body, fitness::day_names[index], Action::Plan, static_cast<int>(index));
        break;
    }
    case Page::Plan: planPage(); break;
    case Page::ConfirmEnd: {
        auto *body = frame("提前结束？");
        label(body, "保存已完成的组数。\n未完成动作会标记为未完成。", 244);
        button(body, "确认结束并保存", Action::Finish);
        button(body, "继续训练", Action::Page, static_cast<int>(Page::Training));
        break;
    }
    }
}
bool FitnessView::back()
{
    switch (page_) {
    case Page::Home: if (home_) home_(context_); return true;
    case Page::Cues: case Page::ConfirmEnd: show(Page::Training); break;
    case Page::Goal: case Page::Templates: show(Page::Settings); break;
    case Page::Plan: show(Page::Templates); break;
    case Page::Record: show(Page::History); break;
    default: show(Page::Home); break;
    }
    return true;
}
void FitnessView::onEvent(lv_event_t *event)
{
    const auto binding = *static_cast<Binding *>(lv_event_get_user_data(event));
    binding.view->act(binding.action, binding.argument);
}
void FitnessView::act(Action action, int argument)
{
    auto goal = model_.state().settings.goals[exercise_];
    switch (action) {
    case Action::Home: if (home_) home_(context_); return;
    case Action::Back: back(); return;
    case Action::Page: show(static_cast<Page>(argument)); return;
    case Action::Day:
        if (!model_.state().session.active) { selected_day_ = argument; manual_day_ = true; show(Page::Home); }
        return;
    case Action::Start:
        if (model_.state().session.active || (selected_day_ >= 0 && model_.start(selected_day_, epoch_, now_))) show(Page::Training);
        return;
    case Action::Count:
        model_.countSet(now_);
        if (!model_.state().session.active) page_ = Page::Summary;
        break;
    case Action::Next: if (model_.next(now_) && !model_.state().session.active) page_ = Page::Summary; break;
    case Action::Undo: model_.undo(now_); break;
    case Action::Pause:
        if (model_.state().session.rest == fitness::Rest::Paused) model_.resumeRest(now_);
        else model_.pauseRest(now_);
        break;
    case Action::Skip: model_.skipRest(now_); break;
    case Action::Finish: model_.finish(now_); page_ = Page::Summary; break;
    case Action::Record: record_ = argument; show(Page::Record); return;
    case Action::Goal: exercise_ = argument; show(Page::Goal); return;
    case Action::SetsMinus: if (goal.sets > 1) --goal.sets; model_.setGoal(exercise_, goal, now_); break;
    case Action::SetsPlus: if (goal.sets < 10) ++goal.sets; model_.setGoal(exercise_, goal, now_); break;
    case Action::RepsMinus: if (goal.reps > 1) --goal.reps; model_.setGoal(exercise_, goal, now_); break;
    case Action::RepsPlus: if (goal.reps < 50) ++goal.reps; model_.setGoal(exercise_, goal, now_); break;
    case Action::Rest: model_.setRest(argument, now_); break;
    case Action::Plan: plan_day_ = argument; show(Page::Plan); return;
    case Action::Move: case Action::Remove: case Action::Add: case Action::Up: {
        auto plan = model_.state().settings.plans[plan_day_];
        auto &moves = plan.moves;
        if (action == Action::Move || action == Action::Add) {
            size_t slot = argument;
            if (action == Action::Add) {
                slot = 0;
                while (slot < moves.size() && moves[slot] != fitness::no_exercise) ++slot;
            }
            if (slot < moves.size()) {
                int candidate = moves[slot] == fitness::no_exercise ? -1 : moves[slot];
                do { candidate = (candidate + 1) % fitness::exercise_count; }
                while (std::find(moves.begin(), moves.end(), candidate) != moves.end());
                moves[slot] = static_cast<uint8_t>(candidate);
            }
        } else if (action == Action::Up && argument > 0) std::swap(moves[argument], moves[argument - 1]);
        else if (action == Action::Remove) {
            for (size_t index = argument; index + 1 < moves.size(); ++index) moves[index] = moves[index + 1];
            moves.back() = fitness::no_exercise;
        }
        model_.setPlan(plan_day_, plan, now_);
        break;
    }
    case Action::Retry: model_.retrySave(now_); break;
    }
    dirty_ = true;
}
