#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>
#include "dashboard_ui.h"

void saveFrame(gfx::Canvas &canvas, const char *path)
{
    std::ofstream file(path, std::ios::binary);
    file << "P6\n360 360\n255\n";
    for (int i = 0; i < 360 * 360; ++i) {
        const uint16_t value = gfx::swap16(canvas.pixels()[i]);
        const char rgb[] = {static_cast<char>(((value >> 11) & 31) * 255 / 31),
                            static_cast<char>(((value >> 5) & 63) * 255 / 63),
                            static_cast<char>((value & 31) * 255 / 31)};
        file.write(rgb, sizeof(rgb));
    }
}

int main()
{
    assert(backgrounds::dayScene().size == backgrounds::kBytes);
    assert(backgrounds::nightScene().size == backgrounds::kBytes);
    gfx::Canvas canvas;
    assert(canvas.begin());
    auto *first_allocation = canvas.pixels();
    assert(canvas.begin() && canvas.pixels() == first_allocation);
    dashboard::State state;
    state.timeValid = true;
    std::strcpy(state.clock, "14:30");
    std::strcpy(state.date, "02 Oct Fri");
    state.linkHealth = dashboard::LinkHealth::CodexLive;
    state.batteryPercent = 74;
    state.quotaAvailable = true;
    state.fiveHourRemainingPercent = 63;
    state.weeklyRemainingPercent = 72;
    state.fiveHourResetInSeconds = 5400;
    dashboard::render(canvas, state);
    std::vector<uint16_t> original(canvas.pixels(), canvas.pixels() + 360 * 360);
    saveFrame(canvas, ".cache/codex-day.ppm");
    // Exercise the app's big-endian -> native LVGL frame conversion, then redraw.
    for (int i = 0; i < 360 * 360; ++i) canvas.pixels()[i] = gfx::swap16(canvas.pixels()[i]);
    dashboard::render(canvas, state);
    assert(std::memcmp(original.data(), canvas.pixels(), backgrounds::kBytes) == 0);
    state.night = true;
    dashboard::render(canvas, state);
    assert(std::memcmp(original.data(), canvas.pixels(), backgrounds::kBytes) != 0);
    saveFrame(canvas, ".cache/codex-night.ppm");
    state.timeValid = state.quotaAvailable = false;
    state.batteryPercent = -1;
    state.linkHealth = dashboard::LinkHealth::Offline;
    for (int direction = -1; direction < 4; ++direction) {
        state.swipeDirection = direction;
        dashboard::render(canvas, state);
    }
    state.powerOverlay = dashboard::PowerOverlay::HoldToPowerOff;
    state.powerHoldProgress = 0.5f;
    dashboard::render(canvas, state);
    // Close/reopen mirrors the Brookesia app's cleanup path; ASan detects leaks.
    for (int i = 0; i < 50; ++i) {
        canvas.release();
        assert(canvas.pixels() == nullptr);
        assert(canvas.begin());
        dashboard::render(canvas, state);
    }
    canvas.release();
    std::cout << "PASS: scene assets, frame byte order, UI states and 50 canvas reopen cycles\n";
}
