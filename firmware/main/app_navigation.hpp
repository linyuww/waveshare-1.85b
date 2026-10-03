#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

enum class AppTarget { Codex, Music, Assistant, Settings };
class AppNavigation {
public:
    static bool initialize();
    static bool request(AppTarget target);
    static bool poll(AppTarget &target);
};
