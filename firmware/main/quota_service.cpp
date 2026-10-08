#include "quota_service.hpp"
#include "quota_http.hpp"
#include "system_service.hpp"
#include <cstdio>
#include <cstring>
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "freertos/task.h"

namespace {
constexpr char TAG[] = "quota_http";
constexpr uint32_t POLL_MS = 60000, RETRY_MS = 10000;
uint32_t nowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
struct Response { char body[2049] = {}; size_t length = 0; bool overflow = false; };
esp_err_t receive(esp_http_client_event_t *event) {
    auto *response = static_cast<Response *>(event->user_data);
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) return ESP_OK;
    if (response->length + event->data_len > sizeof(response->body) - 1) {
        response->overflow = true; return ESP_FAIL;
    }
    memcpy(response->body + response->length, event->data, event->data_len);
    response->length += event->data_len;
    return ESP_OK;
}
}
QuotaService &QuotaService::instance() { static QuotaService service; return service; }
esp_err_t QuotaService::start() {
    if (mutex_) return ESP_ERR_INVALID_STATE;
    mutex_ = xSemaphoreCreateMutex();
    if (!mutex_) return ESP_ERR_NO_MEM;
    status_.waitingSinceMs = nowMs();
    return xTaskCreate(taskEntry, "quota_http", 6144, this, 3, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
QuotaService::Snapshot QuotaService::snapshot() {
    if (!mutex_) return {};
    xSemaphoreTake(mutex_, portMAX_DELAY); auto copy = status_; xSemaphoreGive(mutex_); return copy;
}
void QuotaService::taskEntry(void *arg) { static_cast<QuotaService *>(arg)->worker(); }
void QuotaService::worker() {
    uint32_t due = 0;
    bool connected = false;
    char previousGateway[16] = {};
    for (;;) {
        auto *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip{};
        const bool online = SystemService::instance().snapshot().state == SystemService::NetworkState::Connected &&
            netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.gw.addr;
        char gateway[16] = {};
        if (online) snprintf(gateway, sizeof(gateway), IPSTR, IP2STR(&ip.gw));
        if (online != connected || strcmp(gateway, previousGateway)) {
            connected = online; snprintf(previousGateway, sizeof(previousGateway), "%s", gateway);
            xSemaphoreTake(mutex_, portMAX_DELAY);
            status_.wifiConnected = online; ++status_.revision;
            snprintf(status_.endpoint, sizeof(status_.endpoint), online ? "http://%s:8787/quota" : "", gateway);
            snprintf(status_.message, sizeof(status_.message), "%s", online ? "等待配额" : "等待 Wi-Fi");
            if (online) status_.waitingSinceMs = nowMs();
            xSemaphoreGive(mutex_);
            due = 0;
        }
        const uint32_t now = nowMs();
        if (online && (!due || static_cast<int32_t>(now - due) >= 0)) {
            char endpoint[64]; snprintf(endpoint, sizeof(endpoint), "http://%s:8787/quota", gateway);
            const bool success = fetch(endpoint);
            due = nowMs() + (success ? POLL_MS : RETRY_MS);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
bool QuotaService::fetch(const char *url) {
    Response response;
    esp_http_client_config_t config{};
    config.url = url; config.timeout_ms = 22000; config.buffer_size = 1024;
    config.event_handler = receive; config.user_data = &response;
    config.disable_auto_redirect = true;
    auto *client = esp_http_client_init(&config);
    if (!client) return false;
    const esp_err_t error = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    auto *root = error == ESP_OK && status == 200 && !response.overflow
        ? cJSON_ParseWithLengthOpts(response.body, response.length + 1, nullptr, true) : nullptr;
    quota_http::Value value;
    const bool valid = quota_http::parse(root, value);
    cJSON_Delete(root);
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (valid) {
        status_.fiveHourRemainingPercent = value.fiveHourRemainingPercent;
        status_.weeklyRemainingPercent = value.weeklyRemainingPercent;
        status_.fiveHourResetInSeconds = value.fiveHourResetInSeconds;
        status_.weeklyResetInSeconds = value.weeklyResetInSeconds;
        status_.countdownAtMs = nowMs();
        status_.receivedAtMs = status_.countdownAtMs - value.ageMs; status_.available = true;
        snprintf(status_.message, sizeof(status_.message), "已更新");
    } else snprintf(status_.message, sizeof(status_.message), "配额请求失败");
    ++status_.revision;
    xSemaphoreGive(mutex_);
    if (valid) ESP_LOGI(TAG, "updated via HTTP: 5h=%.1f weekly=%.1f age=%lums", value.fiveHourRemainingPercent, value.weeklyRemainingPercent, static_cast<unsigned long>(value.ageMs));
    else ESP_LOGW(TAG, "request failed: error=%s HTTP=%d valid=%d", esp_err_to_name(error), status, valid);
    return valid;
}
