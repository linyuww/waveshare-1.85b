#include "bluetooth_service.hpp"

#include <cstdio>
#include <cstring>
#include "battery.h"
#include "board_i2c.h"
#include "chime.h"
#include "dashboard_ui.h"
#include "esp_bt_device.h"
#include "esp_gap_ble_api.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {
constexpr char TAG[] = "bluetooth";
const char *KEYS[] = {"AG00", "AG01", "AG02", "AG03", "AG04", "AG05", "ACT12", "ACT09", "ACT10"};
uint32_t nowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
class Lock {
public:
    explicit Lock(SemaphoreHandle_t mutex) : mutex_(mutex) { xSemaphoreTake(mutex_, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(mutex_); }
private:
    SemaphoreHandle_t mutex_;
};
}

BluetoothService &BluetoothService::instance() { static BluetoothService service; return service; }

esp_err_t BluetoothService::start()
{
    if (mutex_) return ESP_ERR_INVALID_STATE;
    mutex_ = xSemaphoreCreateMutex();
    commands_ = xQueueCreate(32, sizeof(Command));
    if (!mutex_ || !commands_) return ESP_ERR_NO_MEM;
    return xTaskCreatePinnedToCore(taskEntry, "bluetooth_service", 16384, this, 4, nullptr, 1) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

BluetoothService::Snapshot BluetoothService::snapshot() { Lock lock(mutex_); return status_; }
bool BluetoothService::enqueue(Command command) { return xQueueSend(commands_, &command, 0) == pdPASS; }
bool BluetoothService::pulse(Key key) { return enqueue({Action::Pulse, static_cast<int>(key)}); }
bool BluetoothService::holdMic(bool pressed) { return enqueue({Action::Mic, 0, pressed}); }
bool BluetoothService::joystick(touch_gesture::Direction direction, bool pressed) { return enqueue({Action::Joystick, static_cast<int>(direction), pressed}); }
bool BluetoothService::releaseControls() { release_requested_.store(true); return true; }

bool BluetoothService::setEnabled(bool enabled)
{
    Lock lock(mutex_);
    if (!status_.ready || status_.busy) return false;
    status_.busy = true;
    if (enqueue({Action::Enable, 0, enabled})) return true;
    status_.busy = false;
    return false;
}

bool BluetoothService::pairAgain()
{
    Lock lock(mutex_);
    if (!status_.ready || status_.busy) return false;
    status_.busy = true;
    if (enqueue({Action::Pair})) return true;
    status_.busy = false;
    return false;
}

void BluetoothService::setMessage(const char *message) { Lock lock(mutex_); strlcpy(status_.message, message, sizeof(status_.message)); }
void BluetoothService::taskEntry(void *arg) { static_cast<BluetoothService *>(arg)->worker(); }
void BluetoothService::audioEntry(void *arg)
{
    auto queue = static_cast<QueueHandle_t>(arg);
    int agent;
    for (;;) {
        if (xQueueReceive(queue, &agent, portMAX_DELAY) == pdPASS) chime::playCompletion();
    }
}

void BluetoothService::worker()
{
    bool enabled = true;
    esp_err_t error = nvs_flash_init();
    nvs_handle_t storage;
    if (error == ESP_OK && nvs_open("launcher", NVS_READWRITE, &storage) == ESP_OK) {
        uint8_t saved = 1;
        nvs_get_u8(storage, "ble_enabled", &saved);
        enabled = saved != 0;
        nvs_close(storage);
    }
    ESP_LOGI(TAG, "Before BLE: internal=%u largest=%u PSRAM=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    if (error == ESP_OK) error = backend_.begin(enabled);
    initialized_ = error == ESP_OK;
    {
        Lock lock(mutex_);
        status_.enabled = enabled;
        status_.ready = initialized_;
        status_.busy = false;
        status_.quota_waiting_since = nowMs();
        if (!initialized_) snprintf(status_.message, sizeof(status_.message), "蓝牙启动失败：%s", esp_err_to_name(error));
        else strlcpy(status_.message, enabled ? "在电脑中配对 Codex Micro" : "蓝牙已关闭", sizeof(status_.message));
        if (initialized_) {
            const uint8_t *mac = esp_bt_dev_get_address();
            if (mac) snprintf(status_.address, sizeof(status_.address), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }
    }
    if (!initialized_) ESP_LOGE(TAG, "Start failed: %s", esp_err_to_name(error));
    else ESP_LOGI(TAG, "Ready: address=%s internal=%u largest=%u",
                  status_.address,
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));

    // Display start initializes the shared BSP I2C bus before this task is created.
    if (board_i2c::init() == ESP_OK) {
        const esp_err_t gauge_error = battery::init();
        if (gauge_error != ESP_OK) ESP_LOGW(TAG, "Battery unavailable: %s", esp_err_to_name(gauge_error));
        const esp_err_t chime_error = chime::init();
        if (chime_error != ESP_OK) ESP_LOGW(TAG, "Chime unavailable: %s", esp_err_to_name(chime_error));
        else {
            chimes_ = xQueueCreate(1, sizeof(int));
            if (chimes_ && xTaskCreatePinnedToCore(audioEntry, "codex_chime", 4096, chimes_, 2, nullptr, 1) != pdPASS) {
                vQueueDelete(chimes_);
                chimes_ = nullptr;
                ESP_LOGW(TAG, "Chime task allocation failed");
            }
        }
    }
    for (;;) {
        Command command;
        if (xQueueReceive(commands_, &command, pdMS_TO_TICKS(10)) == pdPASS && initialized_) execute(command);
        publishBattery();
        if (!initialized_) continue;
        if (release_requested_.exchange(false)) {
            // No UI can enqueue further controls after its pause callback. Discard
            // queued input so an old press cannot arrive after this release.
            releaseHeld();
            Command pending;
            while (xQueueReceive(commands_, &pending, 0) == pdPASS) {
                if (pending.action == Action::Enable || pending.action == Action::Pair) execute(pending);
            }
        }
        backend_.poll();
        const uint32_t now = nowMs();
        for (int i = 0; i < 9; ++i) {
            if (release_at_[i] && static_cast<int32_t>(now - release_at_[i]) >= 0) {
                backend_.sendKey(KEYS[i], 0, i < 6 ? i : -1);
                release_at_[i] = 0;
            }
        }
        publish();
    }
}

void BluetoothService::releaseHeld()
{
    for (int i = 0; i < 9; ++i) {
        if (release_at_[i]) backend_.sendKey(KEYS[i], 0, i < 6 ? i : -1);
        release_at_[i] = 0;
    }
    if (mic_held_) backend_.sendKey(KEYS[8], 0);
    if (direction_ != touch_gesture::Direction::None) backend_.sendJoystick(touch_gesture::normalizedAngle(direction_), 0);
    mic_held_ = false;
    direction_ = touch_gesture::Direction::None;
}

void BluetoothService::persistEnabled(bool enabled)
{
    nvs_handle_t storage;
    esp_err_t err = nvs_open("launcher", NVS_READWRITE, &storage);
    if (err == ESP_OK) {
        err = nvs_set_u8(storage, "ble_enabled", enabled);
        if (err == ESP_OK) err = nvs_commit(storage);
        nvs_close(storage);
    }
    if (err != ESP_OK) {
        setMessage("蓝牙已调整，但设置保存失败");
        ESP_LOGW(TAG, "Save enabled: %s", esp_err_to_name(err));
    }
}

void BluetoothService::execute(const Command &command)
{
    if (command.action == Action::Enable || command.action == Action::Pair) {
        releaseHeld();
        const bool enabled = command.action == Action::Pair || command.pressed;
        esp_err_t err = backend_.setEnabled(false);
        if (command.action == Action::Pair) {
            // Bond removal starts after the disconnect callbacks have reached the owner.
            const uint32_t until = nowMs() + 3000;
            while (backend_.connected() && static_cast<int32_t>(nowMs() - until) < 0) {
                backend_.poll();
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            if (backend_.connected()) err = ESP_ERR_TIMEOUT;
            else {
                backend_.enterPairingMode();
                const uint32_t bonds_until = nowMs() + 3000;
                while (esp_ble_get_bond_device_num() > 0 && static_cast<int32_t>(nowMs() - bonds_until) < 0) {
                    backend_.poll();
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
                err = esp_ble_get_bond_device_num() > 0 ? ESP_ERR_TIMEOUT : ESP_OK;
            }
        }
        const esp_err_t enabled_error = backend_.setEnabled(enabled);
        if (err == ESP_OK) err = enabled_error;
        {
            Lock lock(mutex_);
            status_.enabled = enabled;
            status_.busy = false;
            if (err != ESP_OK) snprintf(status_.message, sizeof(status_.message), "蓝牙操作失败：%s", esp_err_to_name(err));
            else strlcpy(status_.message, enabled ? "在电脑中配对 Codex Micro" : "蓝牙已关闭，连接已断开", sizeof(status_.message));
        }
        persistEnabled(enabled);
        return;
    }
    if (command.action == Action::Release) { releaseHeld(); return; }
    if (!backend_.enabled() || !backend_.connected()) return;
    switch (command.action) {
    case Action::Pulse:
        if (command.value < 0 || command.value >= 9) break;
        backend_.sendKey(KEYS[command.value], 1, command.value < 6 ? command.value : -1);
        release_at_[command.value] = nowMs() + 60;
        break;
    case Action::Mic:
        backend_.sendKey(KEYS[8], command.pressed ? 1 : 0);
        mic_held_ = command.pressed;
        break;
    case Action::Joystick:
        direction_ = static_cast<touch_gesture::Direction>(command.value);
        backend_.sendJoystick(touch_gesture::normalizedAngle(direction_), command.pressed ? 1 : 0);
        if (!command.pressed) direction_ = touch_gesture::Direction::None;
        break;
    default: break;
    }
}

void BluetoothService::publish()
{
    const uint32_t now = nowMs();
    auto latest = backend_.snapshot();
    const bool advertising = backend_.advertising();
    int completed = -1;
    if (latest.dirty) {
        for (int i = 0; i < 6; ++i) {
            dashboard::ThreadVisual visual;
            visual.color = latest.threads[i].color;
            visual.brightness = latest.threads[i].brightness;
            const int next = static_cast<int>(dashboard::classify(visual));
            if (previous_agents_[i] >= 0 && previous_agents_[i] != next && next == static_cast<int>(dashboard::AgentStatus::Complete)) completed = i;
            previous_agents_[i] = next;
        }
    }
    if (completed >= 0 && backend_.enabled() && chimes_) xQueueOverwrite(chimes_, &completed);
    Lock lock(mutex_);
    if (latest.connectionEpoch != status_.codex.connectionEpoch) {
        status_.quota_waiting_since = now;
        previous_agents_.fill(-1);
    }
    if (latest.dirty || status_.advertising != advertising || status_.connected != latest.connected) ++status_.revision;
    status_.codex = latest;
    status_.connected = latest.connected;
    status_.advertising = advertising;
    if (completed >= 0) { status_.completed_agent = completed; status_.completion_at = now; }
}

void BluetoothService::publishBattery()
{
    const uint32_t now = nowMs();
    if (last_battery_at_ && now - last_battery_at_ < 1000) return;
    last_battery_at_ = now;
    const auto sample = battery::read(now);
    if (initialized_) backend_.setBattery(sample.percent >= 0 ? sample.percent : 0, sample.charging);
    Lock lock(mutex_);
    if (status_.battery_percent != sample.percent || status_.external_power != sample.externalPower) ++status_.revision;
    status_.battery_percent = sample.percent;
    status_.external_power = sample.externalPower;
    status_.battery = sample;
}
