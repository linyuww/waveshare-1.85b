#include "music_service.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include "cJSON.h"
#include "app_navigation.hpp"
#include "assistant_service.hpp"
#include "http_helpers.hpp"
#include "shared_audio.hpp"
#include "system_service.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

namespace {
constexpr int music_sample_rate = 16000;
class Lock {
public:
    explicit Lock(SemaphoreHandle_t mutex) : mutex_(mutex) { xSemaphoreTake(mutex_, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(mutex_); }
private:
    SemaphoreHandle_t mutex_;
};
std::string requestUrl(const char *base, const char *endpoint, const char *song, const char *artist)
{
    std::string result(base);
    while (!result.empty() && result.back() == '/') result.pop_back();
    result += '/';
    result += endpoint;
    result += "?song=" + audio_http::encode(song);
    if (artist[0]) result += "&artist=" + audio_http::encode(artist);
    return result;
}
}

MusicService &MusicService::instance() { static MusicService service; return service; }
esp_err_t MusicService::initialize()
{
    mutex_ = xSemaphoreCreateMutex();
    requests_ = xQueueCreate(1, sizeof(Request));
    if (!mutex_ || !requests_) return ESP_ERR_NO_MEM;
    return xTaskCreateWithCaps(workerEntry, "music_player", 8192, this, 3, nullptr,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
MusicService::Snapshot MusicService::snapshot() { Lock lock(mutex_); return status_; }
bool MusicService::active()
{
    const auto value = snapshot();
    return value.state == State::Resolving || value.state == State::Playing;
}
void MusicService::state(State value, const char *message)
{
    Lock lock(mutex_);
    status_.state = value;
    strlcpy(status_.message, message, sizeof(status_.message));
}
bool MusicService::play(const char *song, const char *artist)
{
    if (!requests_ || !song || !artist || !song[0] || strlen(song) > 120 || strlen(artist) > 120 ||
        !AssistantService::instance().config().music_url[0] ||
        SystemService::instance().snapshot().state != SystemService::NetworkState::Connected) return false;
    Request request = {};
    strlcpy(request.song, song, sizeof(request.song));
    strlcpy(request.artist, artist, sizeof(request.artist));
    request.generation = ++generation_;
    paused_ = false;
    resume_requested_ = false;
    if (xQueueOverwrite(requests_, &request) != pdTRUE) return false;
    AppNavigation::request(AppTarget::Music);
    return true;
}
void MusicService::pause(bool return_to_assistant)
{
    paused_ = true;
    const auto value = snapshot();
    if (value.state == State::Playing || value.state == State::Resolving) state(State::Paused, "音乐已暂停，返回小智继续对话");
    if (return_to_assistant) AppNavigation::request(AppTarget::Assistant);
}
bool MusicService::resume()
{
    if (snapshot().state != State::Paused) return false;
    paused_ = false;
    resume_requested_ = true;
    AppNavigation::request(AppTarget::Music);
    return true;
}

void MusicService::stop(bool return_to_assistant)
{
    ++generation_;
    paused_ = false;
    resume_requested_ = false;
    state(State::Idle, "音乐已停止");
    if (return_to_assistant) AppNavigation::request(AppTarget::Assistant);
}

esp_err_t MusicService::resolve(const Request &request, char *url, size_t size)
{
    const auto config = AssistantService::instance().config();
    const std::string endpoint = requestUrl(config.music_url, "resolve", request.song, request.artist);
    audio_http::Response response;
    esp_http_client_config_t http = {};
    http.url = endpoint.c_str();
    http.timeout_ms = 20000;
    http.crt_bundle_attach = esp_crt_bundle_attach;
    http.event_handler = audio_http::event;
    http.user_data = &response;
    auto client = esp_http_client_init(&http);
    if (!client) return ESP_ERR_NO_MEM;
    const esp_err_t result = esp_http_client_perform(client);
    const int code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    ESP_LOGI("music", "Resolve completed: HTTP %d, result=%s", code, esp_err_to_name(result));
    if (result != ESP_OK || code != 200 || response.overflow) return result == ESP_OK ? ESP_ERR_INVALID_RESPONSE : result;
    auto *root = cJSON_Parse(response.body.c_str());
    if (!cJSON_IsObject(root) || cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(root, "ok"))) { cJSON_Delete(root); return ESP_ERR_INVALID_RESPONSE; }
    const char *stream_url = audio_http::string(root, "url");
    const auto *rate = cJSON_GetObjectItemCaseSensitive(root, "sampleRate");
    if (!rate) rate = cJSON_GetObjectItemCaseSensitive(root, "sample_rate");
    const auto *channels = cJSON_GetObjectItemCaseSensitive(root, "channels");
    if ((rate && (!cJSON_IsNumber(rate) || rate->valueint != music_sample_rate)) ||
        (channels && (!cJSON_IsNumber(channels) || channels->valueint != 1))) { cJSON_Delete(root); return ESP_ERR_NOT_SUPPORTED; }
    const std::string fallback = requestUrl(config.music_url, "stream", request.song, request.artist);
    if (!stream_url[0]) stream_url = fallback.c_str();
    if (!audio_http::validUrl(stream_url) || strlen(stream_url) >= size) { cJSON_Delete(root); return ESP_ERR_INVALID_RESPONSE; }
    strlcpy(url, stream_url, size);
    if (request.generation == generation_.load()) {
        Lock lock(mutex_);
        strlcpy(status_.song, audio_http::string(root, "title", request.song), sizeof(status_.song));
        strlcpy(status_.artist, audio_http::string(root, "artist", request.artist), sizeof(status_.artist));
    }
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t MusicService::stream(const Request &request, const char *url)
{
    audio_http::StreamHeaders headers;
    esp_http_client_config_t http = {};
    http.url = url;
    http.timeout_ms = 30000;
    http.crt_bundle_attach = esp_crt_bundle_attach;
    http.buffer_size = 2048;
    http.event_handler = audio_http::streamEvent;
    http.user_data = &headers;
    auto client = esp_http_client_init(&http);
    if (!client) return ESP_ERR_NO_MEM;
    esp_http_client_set_header(client, "Accept", "audio/pcm, application/octet-stream");
    esp_err_t result = esp_http_client_open(client, 0);
    if (result != ESP_OK) { esp_http_client_cleanup(client); return result; }
    const int64_t length = esp_http_client_fetch_headers(client);
    const int code = esp_http_client_get_status_code(client);
    ESP_LOGI("music", "Stream opened: HTTP %d, PCM=%d", code, audio_http::rawPcm(headers.content_type));
    const auto stream_result = audio_http::musicStreamResult(code, headers.content_type);
    if (length < 0 || stream_result != audio_http::MusicStreamResult::Ready) {
        esp_http_client_cleanup(client);
        if (stream_result == audio_http::MusicStreamResult::Unavailable) return ESP_ERR_NOT_FOUND;
        if (stream_result == audio_http::MusicStreamResult::Busy) return ESP_ERR_INVALID_STATE;
        if (stream_result == audio_http::MusicStreamResult::InvalidFormat) return ESP_ERR_NOT_SUPPORTED;
        return ESP_FAIL;
    }
    esp_http_client_set_timeout_ms(client, 1000);
    auto *buffer = static_cast<uint8_t *>(heap_caps_malloc(2048, MALLOC_CAP_SPIRAM));
    if (!buffer) { esp_http_client_cleanup(client); return ESP_ERR_NO_MEM; }
    uint64_t frames = 0;
    size_t carry = 0;
    unsigned empty_reads = 0;
    bool first = true;
    result = ESP_OK;
    state(paused_.load() ? State::Paused : State::Playing, paused_.load() ? "音乐已暂停" : "正在播放，小智后台聆听暂停命令");
    while (request.generation == generation_.load()) {
        if (paused_.load()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (resume_requested_.exchange(false)) state(State::Playing, "继续播放，小智后台聆听");
        if (SystemService::instance().snapshot().state != SystemService::NetworkState::Connected) { result = ESP_ERR_INVALID_STATE; break; }
        const int bytes = esp_http_client_read(client, reinterpret_cast<char *>(buffer + carry), 2048 - carry);
        if (bytes < 0 && bytes != -ESP_ERR_HTTP_EAGAIN) { result = ESP_FAIL; break; }
        if (bytes <= 0) {
            if (esp_http_client_is_complete_data_received(client)) break;
            if (++empty_reads >= 8) { result = ESP_ERR_TIMEOUT; break; }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        empty_reads = 0;
        const size_t total = carry + bytes;
        if (first && total < 8) {
            carry = total;
            continue;
        }
        if (first) {
            first = false;
            if (memcmp(buffer, "RIFF", 4) == 0 || memcmp(buffer, "OggS", 4) == 0 || memcmp(buffer, "fLaC", 4) == 0 ||
                memcmp(buffer, "ID3", 3) == 0 || buffer[0] == '{' || buffer[0] == '<') { result = ESP_ERR_NOT_SUPPORTED; break; }
        }
        const size_t aligned = total & ~size_t(1);
        if (request.generation != generation_.load()) break;
        while (paused_.load() && request.generation == generation_.load()) vTaskDelay(pdMS_TO_TICKS(20));
        if (request.generation != generation_.load()) break;
        if (aligned && !paused_.load()) {
            result = shared_audio::write(shared_audio::Source::Music, reinterpret_cast<const int16_t *>(buffer), aligned / 2, music_sample_rate);
            if (result != ESP_OK) break;
            frames += aligned / 2;
            Lock lock(mutex_);
            status_.playback_ms = frames * 1000 / music_sample_rate;
        }
        carry = total - aligned;
        if (carry) buffer[0] = buffer[aligned];
    }
    if (carry && result == ESP_OK && request.generation == generation_.load()) result = ESP_ERR_INVALID_SIZE;
    free(buffer);
    esp_http_client_cleanup(client);
    ESP_LOGI("music", "Stream completed: result=%s, frames=%llu", esp_err_to_name(result), static_cast<unsigned long long>(frames));
    return result;
}

void MusicService::workerEntry(void *context) { static_cast<MusicService *>(context)->worker(); }
void MusicService::worker()
{
    Request request;
    auto *url = static_cast<char *>(heap_caps_malloc(2048, MALLOC_CAP_SPIRAM));
    if (!url) { state(State::Error, "音乐缓存内存不足"); vTaskDeleteWithCaps(nullptr); return; }
    while (true) {
        if (xQueueReceive(requests_, &request, portMAX_DELAY) != pdTRUE) continue;
        if (request.generation != generation_.load()) continue;
        { Lock lock(mutex_); strlcpy(status_.song, request.song, sizeof(status_.song)); strlcpy(status_.artist, request.artist, sizeof(status_.artist)); status_.playback_ms = 0; }
        state(State::Resolving, "正在查找歌曲");
        esp_err_t result = resolve(request, url, 2048);
        if (request.generation != generation_.load()) continue;
        if (result == ESP_OK) result = stream(request, url);
        if (request.generation != generation_.load()) continue;
        if (result == ESP_OK) state(State::Idle, "播放结束");
        else if (result == ESP_ERR_NOT_FOUND) state(State::Error, "已找到歌曲，但接口没有可播放音源，请换一首或稍后重试");
        else if (result == ESP_ERR_INVALID_STATE) state(State::Error, "音乐服务忙碌或网络已断开，请稍后重试");
        else if (result == ESP_ERR_NOT_SUPPORTED) state(State::Error, "音乐服务需返回 16kHz 单声道 PCM，不能直接播放 MP3");
        else state(State::Error, "播放失败，请检查共享网络与音乐服务");
    }
}
