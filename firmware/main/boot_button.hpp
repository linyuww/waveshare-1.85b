#pragma once
#include <cstdint>

// LVGL owns this state. Entering with BOOT held requires a debounced release.
class BootButton {
public:
    enum class Edge { None, Press, Release };
    void reset(bool pressed, uint32_t now) {
        raw_ = stable_ = pressed;
        armed_ = !pressed;
        changed_at_ = pressed_at_ = now;
    }
    Edge update(bool pressed, uint32_t now) {
        if (pressed != raw_) { raw_ = pressed; changed_at_ = now; }
        if (now - changed_at_ < 30 || stable_ == raw_) return Edge::None;
        stable_ = raw_;
        if (stable_) {
            if (!armed_) return Edge::None;
            pressed_at_ = now;
            return Edge::Press;
        }
        const bool was_armed = armed_;
        armed_ = true;
        return was_armed ? Edge::Release : Edge::None;
    }
    bool pressed() const { return stable_ && armed_; }
    uint32_t heldMs(uint32_t now) const { return now - pressed_at_; }
private:
    bool raw_ = false;
    bool stable_ = false;
    bool armed_ = false;
    uint32_t changed_at_ = 0;
    uint32_t pressed_at_ = 0;
};
