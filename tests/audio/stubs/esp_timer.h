#pragma once
#include "lvgl.h"
inline int64_t esp_timer_get_time() { return static_cast<int64_t>(lv_tick_get()) * 1000; }
