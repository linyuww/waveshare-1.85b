#pragma once

#include <cstdint>
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

// This service owns Wi-Fi, network settings and SNTP. Applications consume snapshots and enqueue work.
class SystemService {
public:
    static constexpr int MAX_APS = 16;
    enum class NetworkState { Offline, Connecting, Connected, Retry, Error };
    struct AccessPoint {
        char ssid[33];
        int8_t rssi;
        bool secured;
    };
    struct Snapshot {
        NetworkState state = NetworkState::Offline;
        bool ready = false;
        bool scanning = false;
        bool testing = false;
        bool connection_pending = false;
        int brightness = 70;
        int rssi = -100;
        char ssid[33] = {};
        char ip[16] = {};
        char mac[18] = {};
        char message[128] = {};
        char test_result[256] = {};
        int ap_count = 0;
        uint32_t scan_revision = 0;
        AccessPoint aps[MAX_APS] = {};
    };

    static SystemService &instance();
    esp_err_t start();
    Snapshot snapshot();
    bool scan();
    bool connect(const char *ssid, const char *password);
    bool forget();
    bool saveBrightness(int percent);
    bool testInternet();
    static bool timeValid();
    static const char *stateText(NetworkState state);

private:
    enum class CommandType { Scan, Connect, Forget, Brightness, Test };
    struct Command {
        CommandType type;
        char ssid[33] = {};
        char password[65] = {};
        int brightness = 70;
    };
    static void taskEntry(void *arg);
    static void onNetworkEvent(void *arg, esp_event_base_t base, int32_t id, void *data);
    void worker();
    esp_err_t initialize();
    void execute(const Command &command);
    void doScan();
    void doConnect(const Command &command);
    void doForget();
    void doTest();
    void reconcileNetwork();
    void setMessage(NetworkState state, const char *message);
    bool enqueue(const Command &command);
    esp_err_t persistCredentials();
    esp_err_t persistBrightness(int value);
    void scheduleRetry(int reason);

    SemaphoreHandle_t mutex_ = nullptr;
    QueueHandle_t commands_ = nullptr;
    EventGroupHandle_t events_ = nullptr;
    esp_netif_t *netif_ = nullptr;
    Snapshot status_;
    wifi_config_t config_ = {};
    bool want_connection_ = false;
    bool save_on_connect_ = false;
    bool sntp_initialized_ = false;
    int retries_ = 0;
    int64_t retry_at_ = 0;
    int64_t connect_deadline_ = 0;
    int disconnect_reason_ = 0;
};
