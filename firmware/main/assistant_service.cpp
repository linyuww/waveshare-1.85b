#include "assistant_service.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include "app_navigation.hpp"
#include "assistant_console.hpp"
#include "http_helpers.hpp"
#include "local_mcp.hpp"
#include "music_service.hpp"
#include "shared_audio.hpp"
#include "system_service.hpp"
#include "xiaozhi_protocol.hpp"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "nvs.h"

extern const char mcp_tools_start[] asm("_binary_mcp_tools_json_start");
namespace {
constexpr size_t max_message = 16384;
constexpr char TAG[] = "xiaozhi";
constexpr char storage[] = "voice_apps";
class Lock {
public:
    explicit Lock(SemaphoreHandle_t mutex) : mutex_(mutex) { xSemaphoreTake(mutex_, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(mutex_); }
private:
    SemaphoreHandle_t mutex_;
};
bool validConfig(const AssistantService::Config &config)
{
    if (!memchr(config.ota_url, 0, sizeof(config.ota_url)) || !memchr(config.websocket_url, 0, sizeof(config.websocket_url)) ||
        !memchr(config.token, 0, sizeof(config.token)) || !memchr(config.music_url, 0, sizeof(config.music_url))) return false;
    return audio_http::validUrl(config.ota_url) &&
        (!config.websocket_url[0] || audio_http::validUrl(config.websocket_url, true)) &&
        (!config.music_url[0] || audio_http::validUrl(config.music_url)) &&
        !strchr(config.token, '\r') && !strchr(config.token, '\n') && config.volume >= 0 && config.volume <= 100;
}
}

AssistantService &AssistantService::instance()
{
    static AssistantService service;
    return service;
}

esp_err_t AssistantService::initialize()
{
    mutex_ = xSemaphoreCreateMutex();
    commands_ = xQueueCreate(8, sizeof(Command));
    packets_ = xQueueCreate(64, sizeof(Packet));
    capture_pcm_ = static_cast<int16_t *>(heap_caps_malloc(1440 * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    decode_pcm_ = static_cast<int16_t *>(heap_caps_malloc(5760 * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    receive_buffer_ = static_cast<char *>(heap_caps_malloc(max_message + 1, MALLOC_CAP_SPIRAM));
    if (!mutex_ || !commands_ || !packets_ || !capture_pcm_ || !decode_pcm_ || !receive_buffer_) return ESP_ERR_NO_MEM;
    nvs_handle_t handle;
    if (nvs_open(storage, NVS_READWRITE, &handle) == ESP_OK) {
        Config saved;
        size_t size = sizeof(saved);
        if (nvs_get_blob(handle, "config", &saved, &size) == ESP_OK && size == sizeof(saved) && validConfig(saved)) config_ = saved;
        size = sizeof(client_id_);
        if (nvs_get_str(handle, "client_id", client_id_, &size) != ESP_OK) {
            const uint32_t first = esp_random();
            const uint32_t second = esp_random();
            const uint32_t third = esp_random();
            const uint32_t fourth = esp_random();
            snprintf(client_id_, sizeof(client_id_), "%08lx-%04lx-4%03lx-%04lx-%04lx%08lx",
                static_cast<unsigned long>(first), static_cast<unsigned long>(second >> 16), static_cast<unsigned long>(second & 0xfff),
                static_cast<unsigned long>((third >> 16 & 0x3fff) | 0x8000), static_cast<unsigned long>(third & 0xffff), static_cast<unsigned long>(fourth));
            nvs_set_str(handle, "client_id", client_id_);
            nvs_commit(handle);
        }
        nvs_close(handle);
    }
    if (!client_id_[0]) return ESP_ERR_INVALID_STATE;
    shared_audio::setVolume(config_.volume);
    return xTaskCreateWithCaps(workerEntry, "xiaozhi_service", 65536, this, 4, nullptr,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

AssistantService::Config AssistantService::config() { Lock lock(mutex_); return config_; }
AssistantService::Snapshot AssistantService::snapshot() { Lock lock(mutex_); return status_; }
bool AssistantService::enqueue(Action action, int volume)
{
    Command command{action, volume};
    return commands_ && xQueueSend(commands_, &command, 0) == pdTRUE;
}
bool AssistantService::start() { return enqueue(Action::Start); }
bool AssistantService::stop() { return enqueue(Action::Stop); }
bool AssistantService::listen() { return enqueue(Action::Listen); }
bool AssistantService::finishListening() { return enqueue(Action::Finish); }
bool AssistantService::setVolume(int percent) { return percent >= 0 && percent <= 100 && enqueue(Action::Volume, percent); }
bool AssistantService::saveConfig(const Config &value)
{
    if (!validConfig(value)) return false;
    Lock lock(mutex_);
    if (!enqueue(Action::Save)) return false;
    config_ = value;
    return true;
}
void AssistantService::setMessage(const char *message)
{
    ESP_LOGI(TAG, "%s", message);
    Lock lock(mutex_);
    strlcpy(status_.message, message, sizeof(status_.message));
}
esp_err_t AssistantService::persist()
{
    const auto value = config();
    nvs_handle_t handle;
    esp_err_t result = nvs_open(storage, NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    result = nvs_set_blob(handle, "config", &value, sizeof(value));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}

esp_err_t AssistantService::discover(Config &value)
{
    setMessage("正在获取小智连接与激活信息");
    audio_http::Response response;
    const auto network = SystemService::instance().snapshot();
    char device_id[18];
    if (!xiaozhi_protocol::deviceId(network.mac, device_id)) return ESP_ERR_INVALID_STATE;
    const auto *application = esp_app_get_description();
    char body[1400];
    snprintf(body, sizeof(body), "{\"version\":2,\"language\":\"zh-CN\",\"flash_size\":16777216,\"mac_address\":\"%s\",\"uuid\":\"%s\",\"chip_model_name\":\"esp32s3\",\"application\":{\"name\":\"waveshare-launcher\",\"version\":\"%s\",\"idf_version\":\"%s\"},\"board\":{\"type\":\"waveshare-esp32-s3-touch-lcd-1.85b\"},\"display\":{\"width\":360,\"height\":360,\"monochrome\":false},\"ota\":{\"label\":\"factory\"}}", device_id, client_id_, application->version, application->idf_ver);
    esp_http_client_config_t http = {};
    http.url = value.ota_url;
    http.timeout_ms = 8000;
    http.crt_bundle_attach = esp_crt_bundle_attach;
    http.event_handler = audio_http::event;
    http.user_data = &response;
    auto client = esp_http_client_init(&http);
    if (!client) return ESP_ERR_NO_MEM;
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Activation-Version", "1");
    esp_http_client_set_header(client, "Device-Id", device_id);
    esp_http_client_set_header(client, "Client-Id", client_id_);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "User-Agent", "waveshare-launcher/1.0");
    esp_http_client_set_post_field(client, body, strlen(body));
    const esp_err_t result = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    int tls_error = 0, tls_flags = 0;
    const esp_err_t tls_result = esp_http_client_get_and_clear_last_tls_error(client, &tls_error, &tls_flags);
    ESP_LOGI(TAG, "Discovery: result=%s HTTP=%d bytes=%u TLS=0x%x mbedTLS=%d flags=0x%x socket=%d internal=%u",
        esp_err_to_name(result), status, static_cast<unsigned>(response.body.size()), tls_result, tls_error, tls_flags,
        esp_http_client_get_errno(client), static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    esp_http_client_cleanup(client);
    if (result != ESP_OK || status != 200 || response.overflow) return result == ESP_OK ? ESP_ERR_INVALID_RESPONSE : result;
    auto *root = cJSON_Parse(response.body.c_str());
    if (!root) return ESP_ERR_INVALID_RESPONSE;
    const auto *activation = cJSON_GetObjectItemCaseSensitive(root, "activation");
    const char *code = audio_http::string(activation, "code");
    if (*code) {
        Lock lock(mutex_);
        strlcpy(status_.activation, code, sizeof(status_.activation));
        strlcpy(status_.message, "到 xiaozhi.me 输入激活码，完成后再点开始", sizeof(status_.message));
        ESP_LOGI(TAG, "Device activation code: %s", status_.activation);
        cJSON_Delete(root);
        return ESP_ERR_NOT_FINISHED;
    }
    const auto *websocket = cJSON_GetObjectItemCaseSensitive(root, "websocket");
    ESP_LOGI(TAG, "Discovery protocol: websocket=%d mqtt=%d", cJSON_IsObject(websocket), cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(root, "mqtt")));
    const char *url = audio_http::string(websocket, "url");
    const char *token = audio_http::string(websocket, "token");
    const bool valid = strlen(url) < sizeof(value.websocket_url) && strlen(token) < sizeof(value.token) &&
        audio_http::validUrl(url, true) && !strchr(token, '\r') && !strchr(token, '\n');
    if (valid) {
        strlcpy(value.websocket_url, url, sizeof(value.websocket_url));
        strlcpy(value.token, token, sizeof(value.token));
    }
    cJSON_Delete(root);
    return valid ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t AssistantService::connect()
{
    ESP_LOGI(TAG, "Connect requested: internal=%u PSRAM=%u time_valid=%d", static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)), SystemService::timeValid());
    if (socket_) return ESP_ERR_INVALID_STATE;
    if (SystemService::instance().snapshot().state != SystemService::NetworkState::Connected) {
        setMessage("请先在共享设置中连接 Wi-Fi");
        return ESP_ERR_INVALID_STATE;
    }
    auto value = config();
    if (!value.websocket_url[0]) {
        const esp_err_t result = discover(value);
        if (result != ESP_OK) {
            if (result != ESP_ERR_NOT_FINISHED) setMessage("连接信息获取失败，请检查网络或服务地址");
            return result;
        }
    }
    const esp_err_t microphone_result = shared_audio::enableMicrophone();
    ESP_LOGI(TAG, "Microphone: %s", esp_err_to_name(microphone_result));
    if (microphone_result != ESP_OK) {
        setMessage("麦克风初始化失败");
        return ESP_FAIL;
    }
    int error = OPUS_OK;
    encoder_ = opus_encoder_create(16000, 1, OPUS_APPLICATION_VOIP, &error);
    decoder_ = opus_decoder_create(24000, 1, &error);
    if (!encoder_ || !decoder_) { disconnect(); setMessage("语音编解码器内存不足"); return ESP_ERR_NO_MEM; }
    opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(24000));
    opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(1));
    const auto network = SystemService::instance().snapshot();
    char device_id[18];
    if (!xiaozhi_protocol::deviceId(network.mac, device_id)) { disconnect(); return ESP_ERR_INVALID_STATE; }
    char headers[800];
    snprintf(headers, sizeof(headers), "Protocol-Version: 1\r\nDevice-Id: %s\r\nClient-Id: %s\r\n", device_id, client_id_);
    if (value.token[0]) {
        const size_t used = strlen(headers);
        snprintf(headers + used, sizeof(headers) - used, "Authorization: %s%s\r\n", strncmp(value.token, "Bearer ", 7) == 0 ? "" : "Bearer ", value.token);
    }
    esp_websocket_client_config_t websocket = {};
    websocket.uri = value.websocket_url;
    websocket.headers = headers;
    websocket.crt_bundle_attach = esp_crt_bundle_attach;
    websocket.disable_auto_reconnect = true;
    websocket.network_timeout_ms = 6000;
    websocket.task_stack = 6144;
    websocket.buffer_size = 2048;
    socket_ = esp_websocket_client_init(&websocket);
    if (!socket_) { disconnect(); setMessage("连接内存不足，请重试"); return ESP_ERR_NO_MEM; }
    esp_websocket_register_events(socket_, WEBSOCKET_EVENT_ANY, websocketEvent, this);
    const esp_err_t result = esp_websocket_client_start(socket_);
    ESP_LOGI(TAG, "WebSocket start: %s", esp_err_to_name(result));
    if (result != ESP_OK) { disconnect(); setMessage("连接启动失败，请重试"); return result; }
    hello_deadline_ = esp_timer_get_time() + 20000000;
    setMessage("正在连接小智");
    return ESP_OK;
}

void AssistantService::disconnect()
{
    if (socket_) {
        esp_websocket_client_stop(socket_);
        esp_websocket_client_destroy(socket_);
        socket_ = nullptr;
    }
    socket_connected_ = false;
    announced_connected_ = hello_ = listening_ = speaking_ = false;
    hello_deadline_ = speech_deadline_ = 0;
    captured_frames_ = 0;
    shared_audio::prioritize(shared_audio::Source::Assistant, false);
    receive_used_ = 0;
    Packet packet;
    while (xQueueReceive(packets_, &packet, 0) == pdTRUE) free(packet.data);
    if (encoder_) opus_encoder_destroy(encoder_);
    if (decoder_) opus_decoder_destroy(decoder_);
    encoder_ = nullptr;
    decoder_ = nullptr;
    session_id_[0] = 0;
    Lock lock(mutex_);
    status_.connected = status_.listening = status_.speaking = false;
}

void AssistantService::websocketEvent(void *context, esp_event_base_t, int32_t id, void *event)
{
    auto *self = static_cast<AssistantService *>(context);
    if (id != WEBSOCKET_EVENT_DATA) ESP_LOGI(TAG, "WebSocket event: %ld", static_cast<long>(id));
    if (id == WEBSOCKET_EVENT_ERROR && event) {
        const auto &error = static_cast<esp_websocket_event_data_t *>(event)->error_handle;
        ESP_LOGW(TAG, "WebSocket error: type=%d HTTP=%d internal=%u DMA-largest=%u", error.error_type,
            error.esp_ws_handshake_status_code, static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)));
        if (error.error_type == WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGW(TAG, "WebSocket transport: TLS=0x%x stack=%d flags=0x%x socket=%d", error.esp_tls_last_esp_err,
                error.esp_tls_stack_err, error.esp_tls_cert_verify_flags, error.esp_transport_sock_errno);
        }
    }
    if (id == WEBSOCKET_EVENT_CONNECTED) self->socket_connected_ = true;
    else if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_ERROR || id == WEBSOCKET_EVENT_CLOSED) self->socket_connected_ = false;
    else if (id == WEBSOCKET_EVENT_DATA) {
        const auto *data = static_cast<esp_websocket_event_data_t *>(event);
        if (data->op_code == 8) {
            const int code = data->payload_offset == 0 && data->data_len >= 2 ?
                (static_cast<unsigned char>(data->data_ptr[0]) << 8) | static_cast<unsigned char>(data->data_ptr[1]) : 0;
            ESP_LOGW(TAG, "WebSocket peer close: code=%d bytes=%d", code, data->payload_len);
            self->socket_connected_ = false;
            self->receive_failed_ = true;
            return;
        }
        if (data->op_code != 0 && data->op_code != 1 && data->op_code != 2) return;
        if (data->payload_offset == 0 && data->op_code != 0) {
            self->receive_used_ = 0;
            self->receive_opcode_ = data->op_code;
        }
        if (data->data_len < 0 || self->receive_used_ + data->data_len > max_message) { self->receive_failed_ = true; return; }
        memcpy(self->receive_buffer_ + self->receive_used_, data->data_ptr, data->data_len);
        self->receive_used_ += data->data_len;
        if (!data->fin || data->payload_offset + data->data_len != data->payload_len) return;
        auto *copy = static_cast<char *>(heap_caps_malloc(self->receive_used_ + 1, MALLOC_CAP_SPIRAM));
        if (!copy) { self->receive_failed_ = true; return; }
        memcpy(copy, self->receive_buffer_, self->receive_used_);
        copy[self->receive_used_] = 0;
        Packet packet{self->receive_opcode_, self->receive_used_, copy};
        if (xQueueSend(self->packets_, &packet, 0) != pdTRUE) { free(copy); self->receive_failed_ = true; }
        self->receive_used_ = 0;
    }
}

bool AssistantService::send(cJSON *message)
{
    char *text = cJSON_PrintUnformatted(message);
    cJSON_Delete(message);
    const bool result = text && socket_ && socket_connected_.load() &&
        esp_websocket_client_send_text(socket_, text, strlen(text), pdMS_TO_TICKS(1000)) == static_cast<int>(strlen(text));
    free(text);
    return result;
}
bool AssistantService::sendListen(const char *state)
{
    auto *message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "session_id", session_id_);
    cJSON_AddStringToObject(message, "type", "listen");
    cJSON_AddStringToObject(message, "state", state);
    if (strcmp(state, "start") == 0) cJSON_AddStringToObject(message, "mode", "auto");
    return send(message);
}

void AssistantService::handleText(const char *text)
{
    auto *root = cJSON_Parse(text);
    if (!root) return;
    const char *type = audio_http::string(root, "type");
    if (strcmp(type, "hello") == 0 && strcmp(audio_http::string(root, "transport"), "websocket") == 0) {
        const auto *audio = cJSON_GetObjectItemCaseSensitive(root, "audio_params");
        const auto *rate = cJSON_GetObjectItemCaseSensitive(audio, "sample_rate");
        downlink_rate_ = cJSON_IsNumber(rate) ? rate->valueint : 24000;
        if (downlink_rate_ != 8000 && downlink_rate_ != 12000 && downlink_rate_ != 16000 && downlink_rate_ != 24000 && downlink_rate_ != 48000) {
            receive_failed_ = true;
        } else {
            opus_decoder_destroy(decoder_);
            int error = OPUS_OK;
            decoder_ = opus_decoder_create(downlink_rate_, 1, &error);
            strlcpy(session_id_, audio_http::string(root, "session_id"), sizeof(session_id_));
            hello_ = decoder_ && error == OPUS_OK && sendListen("start");
            ESP_LOGI(TAG, "Server hello: sample_rate=%d ready=%d", downlink_rate_, hello_);
            listening_ = hello_;
            hello_deadline_ = 0;
            Lock lock(mutex_);
            status_.connected = hello_;
            status_.activation[0] = 0;
            strlcpy(status_.message, hello_ ? "正在聆听，可说打开 Codex 或播放音乐" : "语音通道初始化失败", sizeof(status_.message));
            if (!hello_) receive_failed_ = true;
        }
    } else if (strcmp(type, "stt") == 0) {
        Lock lock(mutex_);
        strlcpy(status_.recognized, audio_http::string(root, "text"), sizeof(status_.recognized));
    } else if (strcmp(type, "tts") == 0) {
        const char *state = audio_http::string(root, "state");
        if (strcmp(state, "start") == 0) {
            speaking_ = true;
            listening_ = false;
            speech_deadline_ = esp_timer_get_time() + 30000000;
            shared_audio::prioritize(shared_audio::Source::Assistant, true);
        } else if (strcmp(state, "stop") == 0) {
            speaking_ = false;
            listening_ = hello_ && sendListen("start");
            shared_audio::prioritize(shared_audio::Source::Assistant, false);
            speech_deadline_ = 0;
        } else if (strcmp(state, "sentence_start") == 0) {
            Lock lock(mutex_);
            strlcpy(status_.reply, audio_http::string(root, "text"), sizeof(status_.reply));
        }
    } else if (strcmp(type, "mcp") == 0) {
        auto *response = local_mcp::dispatch(cJSON_GetObjectItemCaseSensitive(root, "payload"), mcp_tools_start,
            [this](const char *name, const cJSON *arguments, std::string &message) { return invokeTool(name, arguments, message); });
        if (response) {
            auto *message = cJSON_CreateObject();
            cJSON_AddStringToObject(message, "session_id", session_id_);
            cJSON_AddStringToObject(message, "type", "mcp");
            cJSON_AddItemToObject(message, "payload", response);
            if (!send(message)) receive_failed_ = true;
        }
    } else if (strcmp(type, "goodbye") == 0) {
        receive_failed_ = true;
    }
    cJSON_Delete(root);
}

void AssistantService::handleAudio(const char *data, size_t length)
{
    if (!decoder_ || !hello_ || !speaking_ || length > 4096) return;
    const int count = opus_decode(decoder_, reinterpret_cast<const unsigned char *>(data), length, decode_pcm_, 5760, 0);
    if (count < 0) { receive_failed_ = true; return; }
    speech_deadline_ = esp_timer_get_time() + 30000000;
    if (shared_audio::write(shared_audio::Source::Assistant, decode_pcm_, count, downlink_rate_) != ESP_OK) receive_failed_ = true;
}

void AssistantService::capture()
{
    if (!encoder_ || !hello_ || !listening_ || speaking_) return;
    if (shared_audio::read(capture_pcm_, 1440) != ESP_OK) { receive_failed_ = true; return; }
    for (size_t index = 0; index < 960; ++index) decode_pcm_[index] = capture_pcm_[index * 3 / 2];
    unsigned char encoded[512];
    const int length = opus_encode(encoder_, decode_pcm_, 960, encoded, sizeof(encoded));
    if (length < 0 || esp_websocket_client_send_bin(socket_, reinterpret_cast<const char *>(encoded), length, pdMS_TO_TICKS(1000)) != length) {
        receive_failed_ = true;
        return;
    }
    ++captured_frames_;
    if (captured_frames_ == 1 || captured_frames_ % 100 == 0) {
        ESP_LOGI(TAG, "Voice uplink: frames=%lu stack-free=%u internal=%u DMA-largest=%u",
            static_cast<unsigned long>(captured_frames_), static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)));
    }
}

bool AssistantService::invokeTool(const char *name, const cJSON *arguments, std::string &message)
{
    bool result = false;
    if (strcmp(name, "self.codex.open") == 0) result = AppNavigation::request(AppTarget::Codex);
    else if (strcmp(name, "self.music.open") == 0) result = AppNavigation::request(AppTarget::Music);
    else if (strcmp(name, "self.apps.open") == 0) {
        const char *app = audio_http::string(arguments, "app");
        const auto target = strcmp(app, "codex") == 0 ? AppTarget::Codex : strcmp(app, "music") == 0 ? AppTarget::Music : strcmp(app, "settings") == 0 ? AppTarget::Settings : AppTarget::Assistant;
        result = AppNavigation::request(target);
    } else if (strcmp(name, "self.music.play") == 0) {
        result = MusicService::instance().play(audio_http::string(arguments, "song"), audio_http::string(arguments, "artist"));
    } else if (strcmp(name, "self.music.pause") == 0) {
        MusicService::instance().pause();
        result = true;
    } else if (strcmp(name, "self.music.stop") == 0) {
        MusicService::instance().stop();
        result = true;
    } else if (strcmp(name, "self.audio.set_volume") == 0) {
        result = setVolume(cJSON_GetObjectItemCaseSensitive(arguments, "volume")->valueint);
    } else if (strcmp(name, "self.device.get_status") == 0) {
        const auto network = SystemService::instance().snapshot();
        const auto music = MusicService::instance().snapshot();
        char status[256];
        snprintf(status, sizeof(status), "{\"wifi_connected\":%s,\"assistant_connected\":%s,\"music_state\":%d,\"volume\":%d,\"chime_priority\":true}", network.state == SystemService::NetworkState::Connected ? "true" : "false", hello_ ? "true" : "false", static_cast<int>(music.state), shared_audio::volume());
        message = status;
        return true;
    }
    message = result ? "操作已受理" : "操作失败：请检查共享网络、音乐服务地址或队列状态";
    return result;
}

void AssistantService::workerEntry(void *context) { static_cast<AssistantService *>(context)->worker(); }
void AssistantService::worker()
{
    const esp_err_t console_result = assistant_console::initialize();
    if (console_result != ESP_OK) ESP_LOGW(TAG, "USB diagnostics unavailable: %s", esp_err_to_name(console_result));
    while (true) {
        assistant_console::poll();
        Command command;
        if (xQueueReceive(commands_, &command, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (command.action == Action::Start) {
                disconnect();
                receive_failed_ = false;
                const esp_err_t result = connect();
                if (result != ESP_OK && result != ESP_ERR_NOT_FINISHED && snapshot().message[0] == 0) setMessage("连接失败，请重试");
            } else if (command.action == Action::Stop) {
                disconnect();
                setMessage("小智已停止，音乐和 Codex 提示音不受影响");
            } else if (command.action == Action::Listen) {
                if (hello_) {
                    auto *abort = cJSON_CreateObject();
                    cJSON_AddStringToObject(abort, "type", "abort");
                    cJSON_AddStringToObject(abort, "session_id", session_id_);
                    send(abort);
                    speaking_ = false;
                    shared_audio::prioritize(shared_audio::Source::Assistant, false);
                    listening_ = sendListen("start");
                } else {
                    disconnect();
                    receive_failed_ = false;
                    connect();
                }
            } else if (command.action == Action::Finish) {
                if (hello_) sendListen("stop");
                listening_ = false;
            } else if (command.action == Action::Save) {
                const auto value = config();
                shared_audio::setVolume(value.volume);
                setMessage(persist() == ESP_OK ? "设置已保存，重新开始对话生效" : "设置保存失败");
            } else if (command.action == Action::Volume) {
                if (shared_audio::setVolume(command.volume) == ESP_OK) {
                    { Lock lock(mutex_); config_.volume = command.volume; }
                    if (persist() != ESP_OK) setMessage("音量已调整，但保存失败");
                }
            }
        }
        if (socket_connected_.load() && !announced_connected_) {
            announced_connected_ = true;
            auto *hello = cJSON_CreateObject();
            cJSON_AddStringToObject(hello, "type", "hello");
            cJSON_AddNumberToObject(hello, "version", 1);
            cJSON_AddStringToObject(hello, "transport", "websocket");
            cJSON_AddBoolToObject(cJSON_AddObjectToObject(hello, "features"), "mcp", true);
            auto *audio = cJSON_AddObjectToObject(hello, "audio_params");
            cJSON_AddStringToObject(audio, "format", "opus");
            cJSON_AddNumberToObject(audio, "sample_rate", 16000);
            cJSON_AddNumberToObject(audio, "channels", 1);
            cJSON_AddNumberToObject(audio, "frame_duration", 60);
            if (!send(hello)) receive_failed_ = true;
        }
        Packet packet;
        for (int count = 0; count < 8 && xQueueReceive(packets_, &packet, 0) == pdTRUE; ++count) {
            if (packet.opcode == 1) handleText(packet.data);
            else if (packet.opcode == 2) handleAudio(packet.data, packet.length);
            free(packet.data);
        }
        const int64_t now = esp_timer_get_time();
        if (socket_ && (receive_failed_.exchange(false) || (announced_connected_ && !socket_connected_.load()) ||
            (hello_deadline_ && now > hello_deadline_) || (speech_deadline_ && speaking_ && now > speech_deadline_) ||
            SystemService::instance().snapshot().state != SystemService::NetworkState::Connected)) {
            disconnect();
            setMessage("语音连接已断开，请点击开始重连");
        }
        capture();
        { Lock lock(mutex_); status_.listening = listening_; status_.speaking = speaking_; }
    }
}
