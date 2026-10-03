#pragma once
#include <cassert>
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_ERROR_CHECK(result) assert((result) == ESP_OK)
inline const char *esp_err_to_name(int) { return "stub"; }
