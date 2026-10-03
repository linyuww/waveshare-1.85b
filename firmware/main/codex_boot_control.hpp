#pragma once
#include "boot_button.hpp"

class CodexBootControl {
public:
    void reset(bool pressed, uint32_t now) { button_.reset(pressed, now); mic_ = false; }
    bool mic() const { return mic_; }
    bool pressed() const { return button_.pressed(); }
    template<class Wake, class Hold, class Voice>
    bool update(bool pressed, uint32_t now, Wake wake, Hold hold, Voice voice) {
        const auto edge = button_.update(pressed, now);
        if (edge == BootButton::Edge::Press) wake();
        bool pulsed = false;
        if (edge == BootButton::Edge::Release) {
            if (mic_) { hold(false); mic_ = false; }
            else if (button_.heldMs(now) < 700) pulsed = voice();
        }
        if (button_.pressed() && !mic_ && button_.heldMs(now) >= 700) mic_ = hold(true);
        return pulsed;
    }
private:
    BootButton button_;
    bool mic_ = false;
};
