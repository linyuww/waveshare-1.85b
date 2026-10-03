#pragma once
#include <atomic>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

class MusicService {
public:
    enum class State { Idle, Resolving, Playing, Paused, Error };
    struct Snapshot {
        State state = State::Idle;
        char song[128] = {};
        char artist[128] = {};
        char message[160] = "输入歌名开始播放";
        uint32_t playback_ms = 0;
    };
    static MusicService &instance();
    esp_err_t initialize();
    bool play(const char *song, const char *artist = "");
    void pause(bool return_to_assistant = true);
    void stop(bool return_to_assistant = true);
    bool resume();
    Snapshot snapshot();
    bool active();
private:
    struct Request { char song[128]; char artist[128]; uint32_t generation; };
    static void workerEntry(void *context);
    void worker();
    esp_err_t resolve(const Request &request, char *url, size_t size);
    esp_err_t stream(const Request &request, const char *url);
    void state(State state, const char *message);
    SemaphoreHandle_t mutex_ = nullptr;
    QueueHandle_t requests_ = nullptr;
    Snapshot status_;
    std::atomic<uint32_t> generation_{0};
    std::atomic<bool> paused_{false};
    std::atomic<bool> resume_requested_{false};
};
