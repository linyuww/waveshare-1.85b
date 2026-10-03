#include "boot_button.hpp"
#include "boot_recording.hpp"
#include "codex_boot_control.hpp"
#include <cassert>
#include <cstdio>

int main()
{
    using Edge = BootButton::Edge;
    BootButton button;
    button.reset(false, 0);
    assert(button.update(true, 10) == Edge::None);
    assert(button.update(false, 20) == Edge::None);
    assert(button.update(true, 25) == Edge::None);
    assert(button.update(true, 54) == Edge::None);
    assert(button.update(true, 55) == Edge::Press);
    assert(button.update(true, 100) == Edge::None);
    assert(button.update(false, 110) == Edge::None);
    assert(button.update(true, 120) == Edge::None);
    assert(button.update(false, 130) == Edge::None);
    assert(button.update(false, 160) == Edge::Release);
    button.reset(true, 200);
    assert(!button.pressed());
    assert(button.update(false, 220) == Edge::None);
    assert(button.update(false, 250) == Edge::None);
    assert(button.update(true, 260) == Edge::None);
    assert(button.update(true, 290) == Edge::Press);
    button.reset(false, UINT32_MAX - 20);
    assert(button.update(true, UINT32_MAX - 10) == Edge::None);
    assert(button.update(true, 20) == Edge::Press);

    CodexBootControl codex;
    unsigned wakes = 0, voices = 0, holds = 0, releases = 0;
    bool accept = true;
    auto sample = [&](bool pressed, uint32_t now) {
        return codex.update(pressed, now, [&] { ++wakes; }, [&](bool down) {
            if (!accept) return false;
            if (down) ++holds; else ++releases;
            return true;
        }, [&] { if (!accept) return false; ++voices; return true; });
    };
    codex.reset(false, 0);
    sample(true, 10); sample(true, 40);
    assert(wakes == 1 && holds == 0 && voices == 0);
    sample(false, 100);
    assert(sample(false, 130) && voices == 1);
    sample(true, 200); sample(true, 230); sample(true, 929);
    assert(!codex.mic());
    sample(true, 930); sample(true, 1000);
    assert(codex.mic() && holds == 1);
    sample(false, 1010); sample(false, 1040);
    assert(!codex.mic() && releases == 1 && voices == 1);
    // A failed hold does not leave a held UI state; subsequent samples retry.
    sample(true, 1100); sample(true, 1130);
    accept = false; sample(true, 1830);
    assert(!codex.mic() && holds == 1);
    accept = true; sample(true, 1880);
    assert(codex.mic() && holds == 2);
    // Leaving discards the gesture without generating a short press.
    codex.reset(true, 1900);
    sample(true, 3000); sample(false, 3010); sample(false, 3040);
    assert(!codex.mic() && holds == 2 && voices == 1);
    sample(true, 3100); sample(true, 3130); sample(false, 3200);
    accept = false;
    assert(!sample(false, 3230) && voices == 1);

    BootRecording recording;
    const auto first = recording.next();
    assert(recording.claim(first));
    recording.finish(first);
    assert(recording.takeFinish());
    assert(!recording.takeFinish()); // pause then close cannot finish twice
    const auto second = recording.next();
    recording.finish(second); // release before worker dequeues the press
    assert(!recording.claim(second));
    const auto third = recording.next();
    assert(recording.claim(third));
    recording.cancel(); // a later touch/console action owns the background session
    recording.finish(third);
    assert(!recording.takeFinish());
    const auto fourth = recording.next();
    assert(recording.claim(fourth));
    recording.cancel(); // connect resets transport state
    recording.finish(fourth); // release while connect is waiting
    recording.restore(fourth);
    assert(recording.takeFinish());
    const auto fifth = recording.next();
    assert(recording.claim(fifth));
    assert(!recording.takeFinish());
    recording.finish(fifth);
    assert(recording.takeFinish());
    puts("PASS: debounce, entry protection, 700ms Codex hold, queue failures and recording ownership");
}
