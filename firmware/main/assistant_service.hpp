#pragma once
#include <atomic>
#include <string>
#include "cJSON.h"
#include "esp_err.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "opus.h"

class AssistantService {
public:
    struct Config {
        char ota_url[256] = "https://api.tenclass.net/xiaozhi/ota/";
        char websocket_url[256] = {};
        char token[512] = {};
        char reserved_music_url[256] = {};
        int volume = 60;
    };
    static_assert(sizeof(Config) == 1284, "Preserve the saved assistant configuration layout");
    struct Snapshot {
        bool connected = false;
        bool listening = false;
        bool speaking = false;
        char message[160] = "点击开始对话";
        char activation[48] = {};
        char recognized[256] = {};
        char reply[384] = {};
    };
    static AssistantService &instance();
    esp_err_t initialize();
    Config config();
    Snapshot snapshot();
    bool saveConfig(const Config &config);
    bool start();
    bool stop();
    bool toggleChat();
    bool setVolume(int percent);
    bool invokeTool(const char *name, const cJSON *arguments, std::string &message);

private:
    enum class Action { Start, Stop, Listen, Toggle, Save, Volume };
    struct Command { Action action; int volume = 0; };
    struct Packet { int opcode; size_t length; char *data; };
    static void workerEntry(void *context);
    static void websocketEvent(void *context, esp_event_base_t base, int32_t id, void *event);
    void worker();
    bool enqueue(Action action, int volume = 0);
    void setMessage(const char *message);
    esp_err_t persist();
    esp_err_t discover(Config &config);
    esp_err_t connect();
    void disconnect();
    bool send(cJSON *message);
    bool sendListen(const char *state);
    void handleText(const char *text);
    void handleAudio(const char *data, size_t length);
    void capture();
    SemaphoreHandle_t mutex_ = nullptr;
    QueueHandle_t commands_ = nullptr;
    QueueHandle_t packets_ = nullptr;
    Config config_;
    Snapshot status_;
    esp_websocket_client_handle_t socket_ = nullptr;
    OpusEncoder *encoder_ = nullptr;
    OpusDecoder *decoder_ = nullptr;
    int16_t *capture_pcm_ = nullptr;
    int16_t *decode_pcm_ = nullptr;
    char *receive_buffer_ = nullptr;
    size_t receive_used_ = 0;
    int receive_opcode_ = 0;
    std::atomic<bool> socket_connected_{false};
    std::atomic<bool> receive_failed_{false};
    bool announced_connected_ = false;
    bool hello_ = false;
    bool listening_ = false;
    bool speaking_ = false;
    bool aborted_ = false;
    int downlink_rate_ = 24000;
    uint32_t captured_frames_ = 0;
    int64_t hello_deadline_ = 0;
    int64_t speech_deadline_ = 0;
    char session_id_[128] = {};
    char client_id_[48] = {};
};
