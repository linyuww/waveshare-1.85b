#pragma once
#include "esp_err.h"
namespace assistant_console {
esp_err_t initialize();
void poll();
}
