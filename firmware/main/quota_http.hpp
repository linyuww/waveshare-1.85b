#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include "cJSON.h"

namespace quota_http {
struct Value {
    float fiveHourRemainingPercent = 0, weeklyRemainingPercent = 0;
    uint32_t fiveHourResetInSeconds = 0, weeklyResetInSeconds = 0;
    uint32_t ageMs = 0;
};
inline bool number(const cJSON *root, const char *key, double low, double high, double &out) {
    const auto *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) || item->valuedouble < low || item->valuedouble > high) return false;
    out = item->valuedouble;
    return true;
}
inline bool parse(const cJSON *root, Value &out) {
    const auto *status = cJSON_GetObjectItemCaseSensitive(root, "status");
    if (!cJSON_IsObject(root) || !cJSON_IsString(status) || strcmp(status->valuestring, "ok")) return false;
    double five, weekly, fiveReset, weeklyReset, age;
    if (!number(root, "five_hour_remaining_percent", 0, 100, five) ||
        !number(root, "weekly_remaining_percent", 0, 100, weekly) ||
        !number(root, "five_hour_reset_in_seconds", 0, 604800, fiveReset) ||
        !number(root, "weekly_reset_in_seconds", 0, 604800, weeklyReset) ||
        !number(root, "age_seconds", 0, 180, age)) return false;
    if (floor(fiveReset) != fiveReset || floor(weeklyReset) != weeklyReset) return false;
    out = {static_cast<float>(five), static_cast<float>(weekly), static_cast<uint32_t>(fiveReset),
           static_cast<uint32_t>(weeklyReset), static_cast<uint32_t>(age * 1000)};
    return true;
}
}
