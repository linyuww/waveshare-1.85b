#pragma once
#include <cstring>
#include <string>
#include <strings.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"

namespace audio_http {
inline bool validUrl(const char *url, bool websocket = false)
{
    if (!url || strchr(url, '\r') || strchr(url, '\n')) return false;
    const char *begin = nullptr;
    if (websocket) {
        if (strncmp(url, "wss://", 6) == 0) begin = url + 6;
        else if (strncmp(url, "ws://", 5) == 0) begin = url + 5;
    } else {
        if (strncmp(url, "https://", 8) == 0) begin = url + 8;
        else if (strncmp(url, "http://", 7) == 0) begin = url + 7;
    }
    return begin && *begin && *begin != '/' && !strchr(begin, ' ');
}
inline std::string encode(const char *value)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char *cursor = reinterpret_cast<const unsigned char *>(value); *cursor; ++cursor) {
        const unsigned char byte = *cursor;
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' || byte == '~') result += byte;
        else { result += '%'; result += hex[byte >> 4]; result += hex[byte & 15]; }
    }
    return result;
}
struct Response { std::string body; bool overflow = false; };
struct StreamHeaders { std::string content_type; };
inline esp_err_t streamEvent(esp_http_client_event_t *event)
{
    auto *headers = static_cast<StreamHeaders *>(event->user_data);
    if (headers && event->event_id == HTTP_EVENT_ON_HEADER && event->header_key && event->header_value &&
        strcasecmp(event->header_key, "Content-Type") == 0) {
        headers->content_type = strlen(event->header_value) <= 256 ? event->header_value : "";
    }
    return ESP_OK;
}
inline bool rawPcm(const std::string &content_type)
{
    const size_t separator = content_type.find(';');
    std::string media_type = content_type.substr(0, separator);
    const size_t last = media_type.find_last_not_of(" \t");
    if (last == std::string::npos) return false;
    media_type.erase(last + 1);
    media_type.erase(0, media_type.find_first_not_of(" \t"));
    return strcasecmp(media_type.c_str(), "audio/pcm") == 0 ||
        strcasecmp(media_type.c_str(), "audio/x-raw") == 0 ||
        strcasecmp(media_type.c_str(), "application/octet-stream") == 0;
}
inline esp_err_t event(esp_http_client_event_t *event)
{
    auto *response = static_cast<Response *>(event->user_data);
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        if (response->body.size() + event->data_len > 16384) {
            response->overflow = true;
            return ESP_ERR_INVALID_SIZE;
        }
        response->body.append(static_cast<const char *>(event->data), event->data_len);
    }
    return ESP_OK;
}
inline const char *string(const cJSON *root, const char *name, const char *fallback = "")
{
    const auto *value = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsString(value) ? value->valuestring : fallback;
}
}
