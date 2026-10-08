#include "quota_http.hpp"
#include "logic.h"
#include <cassert>
#include <cstdio>

int main() {
    const char *wire = R"({"status":"ok","five_hour_remaining_percent":80,"five_hour_reset_in_seconds":3570,"weekly_remaining_percent":65,"weekly_reset_in_seconds":86370,"age_seconds":30})";
    auto *root = cJSON_Parse(wire);
    quota_http::Value value;
    assert(quota_http::parse(root, value));
    assert(value.fiveHourRemainingPercent == 80 && value.weeklyRemainingPercent == 65 && value.ageMs == 30000);
    cJSON_SetNumberValue(cJSON_GetObjectItem(root, "age_seconds"), 181);
    assert(!quota_http::parse(root, value));
    cJSON_SetNumberValue(cJSON_GetObjectItem(root, "age_seconds"), 0);
    cJSON_SetNumberValue(cJSON_GetObjectItem(root, "weekly_reset_in_seconds"), 1.5);
    assert(!quota_http::parse(root, value));
    cJSON_SetNumberValue(cJSON_GetObjectItem(root, "weekly_reset_in_seconds"), 999999999999.0);
    assert(!quota_http::parse(root, value));
    cJSON_SetNumberValue(cJSON_GetObjectItem(root, "weekly_reset_in_seconds"), 60);
    cJSON_SetNumberValue(cJSON_GetObjectItem(root, "five_hour_remaining_percent"), -1);
    assert(!quota_http::parse(root, value));
    cJSON_Delete(root);
    assert(!quota_http::parse(nullptr, value));
    connection_health::Input input;
    input.bleConnected = false; input.quotaAvailable = true;
    input.quotaReceivedAtMs = 1000;
    auto health = connection_health::evaluate(input, 2000, 30000, 180000);
    assert(health.link == connection_health::Link::Offline && health.quota == connection_health::Quota::Fresh);
    assert(connection_health::evaluate(input, 200000, 30000, 180000).quota == connection_health::Quota::Stale);
    input.quotaAvailable = false; input.quotaSourceConnected = true; input.quotaWaitingSinceMs = 0;
    assert(connection_health::evaluate(input, 200000, 30000, 180000).quota == connection_health::Quota::Stale);
    puts("PASS: HTTP quota validation, stale handling and independence from HID");
}
