#pragma once
#include "lvgl.h"
namespace esp_brookesia::systems::phone {
class App {
public:
    App(const char *, const void *, bool, bool, bool) {}
    virtual ~App() = default;
    virtual bool run() = 0;
    virtual bool back() = 0;
    virtual bool close() = 0;
protected:
    bool notifyCoreClosed() { return true; }
};
}
