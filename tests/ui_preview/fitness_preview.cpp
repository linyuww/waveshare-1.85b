#include "fitness_view.hpp"
#include "fitness_assets.h"
#include "ui_assets.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

class PreviewStorage final : public fitness::Storage {
public:
    bool fail = false;
    std::vector<uint8_t> bytes;
    Load read(std::vector<uint8_t> &output) override { output = bytes; return bytes.empty() ? Load::Missing : Load::Loaded; }
    bool write(const std::vector<uint8_t> &input) override { if (fail) return false; bytes = input; return true; }
};
static uint16_t buffer[360 * 360];
static uint16_t pixels[360 * 360];
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *source)
{
    auto *input = reinterpret_cast<uint16_t *>(source);
    for (int vertical = area->y1; vertical <= area->y2; ++vertical)
        for (int horizontal = area->x1; horizontal <= area->x2; ++horizontal) pixels[vertical * 360 + horizontal] = *input++;
    lv_display_flush_ready(display);
}
static void advance(int milliseconds)
{
    for (int elapsed = 0; elapsed < milliseconds; elapsed += 10) { lv_tick_inc(10); lv_timer_handler(); }
}
static void checkGlyphs(const char *text)
{
    const auto *cursor = reinterpret_cast<const unsigned char *>(text);
    while (*cursor) {
        uint32_t codepoint = *cursor++;
        if (codepoint >= 0xC0) {
            int remaining = codepoint < 0xE0 ? 1 : codepoint < 0xF0 ? 2 : 3;
            codepoint &= (1u << (6 - remaining)) - 1;
            while (remaining--) codepoint = (codepoint << 6) | (*cursor++ & 63);
        }
        if (codepoint < 33) continue;
        lv_font_glyph_dsc_t glyph{};
        const bool found = lv_font_get_glyph_dsc(&ui_font, &glyph, codepoint, 0);
        if (!found || glyph.is_placeholder) fprintf(stderr, "Missing fitness glyph U+%04X\n", codepoint);
        assert(found && !glyph.is_placeholder);
    }
}
static lv_obj_t *findButton(lv_obj_t *parent, const char *caption)
{
    for (uint32_t index = 0; index < lv_obj_get_child_count(parent); ++index) {
        auto *child = lv_obj_get_child(parent, index);
        if (lv_obj_check_type(child, &lv_label_class) && strcmp(lv_label_get_text(child), caption) == 0 &&
            lv_obj_check_type(parent, &lv_button_class)) return parent;
        if (auto *found = findButton(child, caption)) return found;
    }
    return nullptr;
}
static void click(FitnessView &view, const char *caption, bool redraw = true)
{
    auto *button = findButton(lv_screen_active(), caption);
    assert(button);
    lv_obj_send_event(button, LV_EVENT_CLICKED, nullptr);
    if (redraw) view.refresh();
}
static void inspect(lv_obj_t *object)
{
    if (lv_obj_check_type(object, &lv_label_class)) checkGlyphs(lv_label_get_text(object));
    for (uint32_t index = 0; index < lv_obj_get_child_count(object); ++index) inspect(lv_obj_get_child(object, index));
}
static void inside(lv_obj_t *object)
{
    assert(object);
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    for (int horizontal : {area.x1, area.x2}) for (int vertical : {area.y1, area.y2})
        assert(std::hypot(horizontal - 179.5, vertical - 179.5) < 180);
}
static void save(const std::string &path)
{
    lv_obj_update_layout(lv_screen_active());
    lv_refr_now(nullptr);
    inspect(lv_screen_active());
    inside(findButton(lv_screen_active(), "桌面"));
    inside(findButton(lv_screen_active(), "返回"));
    auto *file = fopen(path.c_str(), "wb");
    assert(file);
    fprintf(file, "P6\n360 360\n255\n");
    for (auto pixel : pixels) {
        unsigned char rgb[] = {static_cast<unsigned char>(((pixel >> 11) & 31) * 255 / 31),
            static_cast<unsigned char>(((pixel >> 5) & 63) * 255 / 63), static_cast<unsigned char>((pixel & 31) * 255 / 31)};
        fwrite(rgb, 1, 3, file);
    }
    fclose(file);
}
static size_t timers()
{
    size_t count = 0;
    for (auto *timer = lv_timer_get_next(nullptr); timer; timer = lv_timer_get_next(timer)) ++count;
    return count;
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    lv_init();
    auto *display = lv_display_create(360, 360);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buffer, nullptr, sizeof(buffer), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush);
    auto *screen = lv_screen_active();
    const auto baseline_timers = timers();
    for (size_t index = 0; index < fitness::exercise_count; ++index) {
        checkGlyphs(fitness::exercises[index].name);
        checkGlyphs(fitness::exercises[index].cue);
        auto *gif = lv_gif_create(screen);
        lv_gif_set_color_format(gif, LV_COLOR_FORMAT_RGB565);
        lv_gif_set_src(gif, &fitness_gifs[index]);
        assert(lv_gif_is_loaded(gif));
        lv_obj_update_layout(screen);
        assert(lv_obj_get_width(gif) == 200 && lv_obj_get_height(gif) == 120);
        std::set<int32_t> decoded;
        for (int elapsed = 0; elapsed < 4300; elapsed += 10) {
            advance(10);
            decoded.insert(lv_gif_get_current_frame_index(gif));
        }
        assert(decoded.size() >= 24);
        assert(lv_gif_is_loaded(gif));
        lv_obj_delete(gif);
        assert(timers() == baseline_timers);
        printf("GIF %zu: all frames decoded, loop replayed, timer released\n", index);
    }
    PreviewStorage storage;
    fitness::Model model(storage);
    assert(model.load());
    FitnessView view(model, nullptr, nullptr);
    view.attach(screen);
    view.tick(0, 6, 1791000000);
    const std::string directory = argv[1];
    save(directory + "/fitness-home.ppm");
    click(view, "选择其他训练日");
    click(view, "周一 · 胸");
    click(view, "开始训练");
    advance(100);
    lv_obj_update_layout(screen);
    inside(findButton(screen, "完成一组"));
    inside(findButton(screen, "要点"));
    inside(findButton(screen, "撤销"));
    inside(findButton(screen, "结束"));
    save(directory + "/fitness-training.ppm");
    click(view, "完成一组", false);
    click(view, "完成一组", false);
    view.tick(100, 6, 1791000000);
    assert(model.state().session.record.done[0] == 1);
    save(directory + "/fitness-rest.ppm");
    click(view, "暂停");
    assert(model.state().session.rest == fitness::Rest::Paused);
    click(view, "继续");
    view.tick(10000, 6, 1791000000);
    click(view, "撤销上一组");
    assert(model.state().session.record.done[0] == 0);
    click(view, "要点");
    save(directory + "/fitness-cues.ppm");
    click(view, "返回训练");
    view.forceMediaFailure(true);
    view.refresh();
    save(directory + "/fitness-gif-failure.ppm");
    storage.fail = true;
    click(view, "完成一组");
    assert(model.saveFailed());
    save(directory + "/fitness-save-failure.ppm");
    storage.fail = false;
    click(view, "保存失败 · 点此重试");
    assert(!model.saveFailed());
    click(view, "提前结束");
    click(view, "确认结束并保存");
    save(directory + "/fitness-summary.ppm");
    click(view, "回到健身首页");
    click(view, "历史");
    save(directory + "/fitness-history.ppm");
    click(view, "周一 · 胸 · 部分完成");
    save(directory + "/fitness-record.ppm");
    view.show(FitnessView::Page::Settings);
    view.refresh();
    save(directory + "/fitness-settings.ppm");
    click(view, "平板卧推");
    click(view, "组数 +");
    click(view, "次数 -");
    assert(model.state().settings.goals[0].sets == 6 && model.state().settings.goals[0].reps == 11);
    save(directory + "/fitness-goal.ppm");
    click(view, "返回");
    click(view, "调整周计划模板");
    click(view, "周一 · 胸");
    click(view, "更换");
    assert(model.state().settings.plans[0].moves[0] == 2);
    save(directory + "/fitness-plan.ppm");
    view.forceMediaFailure(false);
    for (size_t day = 0; day < fitness::day_count; ++day) {
        assert(model.start(day, 0, 20000));
        for (size_t move = 0; move < model.state().session.record.count; ++move) {
            view.show(FitnessView::Page::Training);
            view.refresh();
            view.releaseMedia();
            assert(timers() == baseline_timers);
            view.show(FitnessView::Page::Training);
            view.tick(20000, 6, 0);
            const auto goal = model.state().session.record.goals[move].sets;
            for (int group = 0; group < goal; ++group) {
                assert(model.countSet(20000));
                if (model.state().session.active) assert(model.skipRest(20000));
            }
            if (model.state().session.active) assert(model.next(20000));
        }
    }
    size_t stable_used = 0;
    for (int iteration = 0; iteration < 100; ++iteration) {
        assert(model.start(4, 0, 40000));
        view.show(FitnessView::Page::Training);
        view.refresh();
        view.releaseMedia();
        assert(timers() == baseline_timers);
        lv_mem_monitor_t memory{};
        lv_mem_monitor(&memory);
        if (iteration == 0) stable_used = memory.total_size - memory.free_size;
        else assert(memory.total_size - memory.free_size == stable_used);
        assert(model.finish(40000));
    }
    view.show(FitnessView::Page::Home);
    view.tick(50000, 0, 0);
    save(directory + "/fitness-unsynced.ppm");
    view.show(FitnessView::Page::History);
    view.refresh();
    inspect(screen);
    assert(model.setPlan(0, {{{0, fitness::no_exercise, fitness::no_exercise, fitness::no_exercise}}}, 50000));
    assert(model.setGoal(0, {10, 50}, 50000));
    assert(model.start(0, 0, 50000));
    view.show(FitnessView::Page::Training);
    view.tick(50000, 1, 0);
    lv_obj_update_layout(screen);
    inside(findButton(screen, "完成一组"));
    inside(findButton(screen, "结束"));
    for (int group = 0; group < 9; ++group) { assert(model.countSet(50000)); assert(model.skipRest(50000)); }
    view.show(FitnessView::Page::Training);
    view.refresh();
    click(view, "完成一组");
    assert(!model.state().session.active && view.page() == FitnessView::Page::Summary);
    assert(model.history(0)->complete && model.history(0)->done[0] == 10);
    view.detach();
    assert(timers() == baseline_timers);
    lv_deinit();
    printf("PASS: production view, Chinese glyphs, circular touch controls, double taps, failures, settings, 100 lifecycle cycles; stable LVGL heap=%zu bytes\n", stable_used);
}
