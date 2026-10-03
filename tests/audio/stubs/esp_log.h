#pragma once
#include <cassert>
#define ESP_LOGW(tag, ...) ((void)(tag))
#include "esp_err.h"
inline const char *esp_err_to_name(int) { return "stub"; }
