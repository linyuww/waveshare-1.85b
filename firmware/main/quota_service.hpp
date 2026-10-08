#pragma once
#include <cstdint>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// HTTP quota has its own lifetime and remains usable with Bluetooth disabled.
class QuotaService {
public:
    struct Snapshot {
        float fiveHourRemainingPercent = 0, weeklyRemainingPercent = 0;
        uint32_t fiveHourResetInSeconds = 0, weeklyResetInSeconds = 0;
        uint32_t receivedAtMs = 0, countdownAtMs = 0, waitingSinceMs = 0, revision = 0;
        bool available = false, wifiConnected = false;
        char endpoint[64] = {};
        char message[64] = "等待 Wi-Fi";
    };
    static QuotaService &instance();
    esp_err_t start();
    Snapshot snapshot();
private:
    static void taskEntry(void *arg);
    void worker();
    bool fetch(const char *url);
    SemaphoreHandle_t mutex_ = nullptr;
    Snapshot status_;
};
