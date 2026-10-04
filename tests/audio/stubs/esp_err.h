#pragma once
#include <algorithm>
#include <cassert>
#define ESP_ERROR_CHECK(call) assert((call) == ESP_OK)
#include <cstring>
using esp_err_t = int;
constexpr int ESP_OK = 0;
constexpr int ESP_FAIL = -1;
constexpr int ESP_ERR_NO_MEM = 1;
constexpr int ESP_ERR_INVALID_STATE = 2;
constexpr int ESP_ERR_INVALID_ARG = 3;
constexpr int ESP_ERR_INVALID_SIZE = 4;
// Avoid redeclaring libc's strlcpy under fortified host builds.
inline size_t preview_strlcpy(char *target, const char *source, size_t size)
{
    const size_t length = strlen(source);
    if (size) { memcpy(target, source, std::min(length, size - 1)); target[std::min(length, size - 1)] = 0; }
    return length;
}
#define strlcpy preview_strlcpy
