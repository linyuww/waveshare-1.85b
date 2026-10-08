#pragma once

#include "codex_ble.h"
#include "battery.h"
#include <atomic>
#include "freertos/task.h"

// One owner task handles BLE, quota, battery and chimes, independently of app screens.
class BluetoothService {
public:
    enum class Key : uint8_t { Agent0, Agent1, Agent2, Agent3, Agent4, Agent5, Send, Voice, Mic };
    struct Snapshot {
        bool ready = false;
        bool enabled = true;
        bool busy = true;
        bool advertising = false;
        bool connected = false;
        bool external_power = false;
        int8_t battery_percent = -1;
        int8_t completed_agent = -1;
        uint32_t completion_at = 0;
        uint32_t revision = 0;
        char address[18] = {};
        char message[128] = "正在启动蓝牙服务";
        battery::Sample battery;
        CodexMicroState codex;
    };

    static BluetoothService &instance();
    esp_err_t start();
    Snapshot snapshot();
    bool setEnabled(bool enabled);
    bool pairAgain();
    bool pulse(Key key);
    bool holdMic(bool pressed);
    bool joystick(touch_gesture::Direction direction, bool pressed);
    bool releaseControls();

private:
    enum class Action : uint8_t { Enable, Pair, Pulse, Mic, Joystick, Release };
    struct Command { Action action; int value = 0; bool pressed = false; };
    static void taskEntry(void *arg);
    static void audioEntry(void *arg);
    void worker();
    void execute(const Command &command);
    bool enqueue(Command command);
    void releaseHeld();
    void publish();
    void publishBattery();
    void setMessage(const char *message);
    void persistEnabled(bool enabled);

    SemaphoreHandle_t mutex_ = nullptr;
    QueueHandle_t commands_ = nullptr;
    QueueHandle_t chimes_ = nullptr;
    Snapshot status_;
    CodexMicroBle backend_;
    bool initialized_ = false;
    uint32_t release_at_[9] = {};
    bool mic_held_ = false;
    touch_gesture::Direction direction_ = touch_gesture::Direction::None;
    uint32_t last_battery_at_ = 0;
    std::array<int, 6> previous_agents_ = {{-1, -1, -1, -1, -1, -1}};
    std::atomic<bool> release_requested_{false};
};
