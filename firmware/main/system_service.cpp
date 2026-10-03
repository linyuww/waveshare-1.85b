#include "system_service.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include "bsp/esp-bsp.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "lwip/netdb.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {
constexpr char TAG[] = "system";
constexpr EventBits_t NETWORK_CHANGED = BIT0;
constexpr char STORAGE[] = "launcher";
class Lock {
public:
    explicit Lock(SemaphoreHandle_t mutex) : mutex_(mutex) { xSemaphoreTake(mutex_, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(mutex_); }
private:
    SemaphoreHandle_t mutex_;
};
}

SystemService &SystemService::instance()
{
    static SystemService service;
    return service;
}

esp_err_t SystemService::start()
{
    if (mutex_) return ESP_ERR_INVALID_STATE;
    mutex_ = xSemaphoreCreateMutex();
    commands_ = xQueueCreate(6, sizeof(Command));
    events_ = xEventGroupCreate();
    if (!mutex_ || !commands_ || !events_) return ESP_ERR_NO_MEM;
    strlcpy(status_.message, "正在启动网络服务", sizeof(status_.message));
    strlcpy(status_.test_result, "点击测试，检查 HTTPS 访问", sizeof(status_.test_result));
    return xTaskCreate(taskEntry, "system_service", 12288, this, 4, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

SystemService::Snapshot SystemService::snapshot()
{
    Lock lock(mutex_);
    return status_;
}

bool SystemService::enqueue(const Command &command)
{
    return xQueueSend(commands_, &command, 0) == pdPASS;
}

bool SystemService::scan()
{
    Lock lock(mutex_);
    if (!status_.ready || status_.scanning || status_.connection_pending ||
        status_.state == NetworkState::Connecting) return false;
    status_.scanning = true;
    if (enqueue({CommandType::Scan})) return true;
    status_.scanning = false;
    return false;
}

bool SystemService::connect(const char *ssid, const char *password)
{
    if (!ssid || !password || strlen(ssid) == 0 || strlen(ssid) > 32 || strlen(password) > 64) return false;
    Command command{CommandType::Connect};
    strlcpy(command.ssid, ssid, sizeof(command.ssid));
    strlcpy(command.password, password, sizeof(command.password));
    Lock lock(mutex_);
    if (!status_.ready || status_.connection_pending) return false;
    status_.connection_pending = true;
    if (enqueue(command)) return true;
    status_.connection_pending = false;
    return false;
}

bool SystemService::forget()
{
    Lock lock(mutex_);
    if (!status_.ready || status_.connection_pending) return false;
    status_.connection_pending = true;
    if (enqueue({CommandType::Forget})) return true;
    status_.connection_pending = false;
    return false;
}

bool SystemService::saveBrightness(int percent)
{
    Command command{CommandType::Brightness};
    command.brightness = std::clamp(percent, 10, 100);
    return enqueue(command);
}

bool SystemService::testInternet()
{
    Lock lock(mutex_);
    if (status_.state != NetworkState::Connected || status_.testing) return false;
    status_.testing = true;
    strlcpy(status_.test_result, "正在进行 HTTPS 请求…", sizeof(status_.test_result));
    if (enqueue({CommandType::Test})) return true;
    status_.testing = false;
    return false;
}

bool SystemService::timeValid() { return time(nullptr) > 1704067200; }

const char *SystemService::stateText(NetworkState state)
{
    switch (state) {
    case NetworkState::Offline: return "未连接";
    case NetworkState::Connecting: return "连接中";
    case NetworkState::Connected: return "已连接";
    case NetworkState::Retry: return "等待重连";
    case NetworkState::Error: return "连接失败";
    }
    return "未知";
}

void SystemService::taskEntry(void *arg) { static_cast<SystemService *>(arg)->worker(); }

void SystemService::onNetworkEvent(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    auto *self = static_cast<SystemService *>(arg);
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        Lock lock(self->mutex_);
        self->disconnect_reason_ = static_cast<wifi_event_sta_disconnected_t *>(data)->reason;
    }
    // Coalesced events are reconciled with the actual netif/AP state in the owner task.
    if ((base == WIFI_EVENT && (id == WIFI_EVENT_STA_DISCONNECTED || id == WIFI_EVENT_STA_START)) ||
        (base == IP_EVENT && (id == IP_EVENT_STA_GOT_IP || id == IP_EVENT_STA_LOST_IP))) {
        xEventGroupSetBits(self->events_, NETWORK_CHANGED);
    }
}

esp_err_t SystemService::initialize()
{
    esp_err_t err = nvs_flash_init();
    // Keep existing credentials intact if NVS is damaged; report the failure for diagnosis.
    if (err != ESP_OK) return err;
    nvs_handle_t storage;
    err = nvs_open(STORAGE, NVS_READWRITE, &storage);
    if (err != ESP_OK) return err;
    size_t size = sizeof(config_);
    if (nvs_get_blob(storage, "wifi", &config_, &size) == ESP_OK && size == sizeof(config_)) {
        want_connection_ = config_.sta.ssid[0] != 0;
    } else {
        memset(&config_, 0, sizeof(config_));
    }
    uint8_t brightness = 70;
    nvs_get_u8(storage, "brightness", &brightness);
    nvs_close(storage);
    {
        Lock lock(mutex_);
        status_.brightness = std::clamp<int>(brightness, 10, 100);
    }
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif initialization failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop failed");
    netif_ = esp_netif_create_default_wifi_sta();
    if (!netif_) return ESP_ERR_NO_MEM;
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "Wi-Fi initialization failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "Wi-Fi RAM storage failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, onNetworkEvent, this), TAG, "Wi-Fi events failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, onNetworkEvent, this), TAG, "IP events failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "station mode failed");
    if (want_connection_) ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &config_), TAG, "saved config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi start failed");
    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_wifi_get_mac(WIFI_IF_STA, mac), TAG, "MAC read failed");
    {
        Lock lock(mutex_);
        snprintf(status_.mac, sizeof(status_.mac), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        status_.ready = true;
        if (want_connection_) snprintf(status_.ssid, sizeof(status_.ssid), "%.32s", reinterpret_cast<const char *>(config_.sta.ssid));
    }
    if (want_connection_) {
        setMessage(NetworkState::Connecting, "正在连接已保存的网络");
        connect_deadline_ = esp_timer_get_time() + 20000000;
        err = esp_wifi_connect();
        if (err != ESP_OK) scheduleRetry(0);
    } else {
        setMessage(NetworkState::Offline, "打开设置，连接 Wi-Fi");
    }
    setenv("TZ", "CST-8", 1);
    tzset();
    return ESP_OK;
}

void SystemService::worker()
{
    esp_err_t err = initialize();
    if (err != ESP_OK) {
        char message[128];
        snprintf(message, sizeof(message), "网络服务启动失败：%s", esp_err_to_name(err));
        setMessage(NetworkState::Error, message);
        ESP_LOGE(TAG, "%s", esp_err_to_name(err));
    }
    bool first_network_test_done = false;
    int64_t first_network_ready_at = 0;
    for (;;) {
        Command command;
        if (xQueueReceive(commands_, &command, pdMS_TO_TICKS(200)) == pdPASS) {
            execute(command);
            // Clear copied credentials from the task stack after use.
            memset(command.password, 0, sizeof(command.password));
        }
        if (!snapshot().ready) continue;
        if (xEventGroupWaitBits(events_, NETWORK_CHANGED, pdTRUE, pdFALSE, 0)) reconcileNetwork();
        const int64_t now = esp_timer_get_time();
        // One initial check populates the network app and gives USB diagnostics.
        // The queue owner performs it after DHCP/SNTP settle; UI and BLE keep running.
        const auto state = snapshot();
        if (!first_network_test_done && state.state == NetworkState::Connected) {
            if (!first_network_ready_at) first_network_ready_at = now;
            if (!state.testing && timeValid() && now - first_network_ready_at >= 5000000) {
                first_network_test_done = testInternet();
            }
        }
        if (want_connection_ && connect_deadline_ && now >= connect_deadline_) {
            esp_wifi_disconnect();
            scheduleRetry(WIFI_REASON_CONNECTION_FAIL);
        }
        if (want_connection_ && retry_at_ && now >= retry_at_) {
            retry_at_ = 0;
            setMessage(NetworkState::Connecting, "正在重新连接 Wi-Fi");
            connect_deadline_ = now + 20000000;
            if (esp_wifi_connect() != ESP_OK) scheduleRetry(0);
        }
    }
}

void SystemService::setMessage(NetworkState state, const char *message)
{
    Lock lock(mutex_);
    status_.state = state;
    strlcpy(status_.message, message, sizeof(status_.message));
    if (state != NetworkState::Connected) status_.ip[0] = 0;
}

void SystemService::execute(const Command &command)
{
    switch (command.type) {
    case CommandType::Scan: doScan(); break;
    case CommandType::Connect: doConnect(command); break;
    case CommandType::Forget: doForget(); break;
    case CommandType::Test: doTest(); break;
    case CommandType::Brightness: {
        const esp_err_t err = persistBrightness(command.brightness);
        Lock lock(mutex_);
        if (err == ESP_OK) status_.brightness = command.brightness;
        else strlcpy(status_.message, "亮度保存失败，请重试", sizeof(status_.message));
        break;
    }
    }
}

void SystemService::doScan()
{
    wifi_scan_config_t scan_config = {};
    scan_config.scan_time.active.max = 120;
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    wifi_ap_record_t records[MAX_APS] = {};
    uint16_t count = MAX_APS;
    if (err == ESP_OK) err = esp_wifi_scan_get_ap_records(&count, records);
    if (err != ESP_OK) esp_wifi_clear_ap_list();
    Lock lock(mutex_);
    status_.ap_count = 0;
    if (err == ESP_OK) {
        for (int i = 0; i < count; ++i) {
            if (!records[i].ssid[0]) continue;
            bool duplicate = false;
            for (int j = 0; j < status_.ap_count; ++j) {
                if (strcmp(status_.aps[j].ssid, reinterpret_cast<char *>(records[i].ssid)) == 0) duplicate = true;
            }
            if (duplicate) continue;
            auto &ap = status_.aps[status_.ap_count++];
            snprintf(ap.ssid, sizeof(ap.ssid), "%.32s", reinterpret_cast<const char *>(records[i].ssid));
            ap.rssi = records[i].rssi;
            ap.secured = records[i].authmode != WIFI_AUTH_OPEN;
        }
        strlcpy(status_.message, status_.ap_count ? "点击网络名称开始配网" : "没有找到网络，可手动输入", sizeof(status_.message));
    } else {
        snprintf(status_.message, sizeof(status_.message), "扫描失败：%s", esp_err_to_name(err));
    }
    status_.scanning = false;
    ++status_.scan_revision;
}

void SystemService::doConnect(const Command &command)
{
    want_connection_ = false;
    retry_at_ = connect_deadline_ = 0;
    esp_wifi_disconnect();
    memset(&config_, 0, sizeof(config_));
    memcpy(config_.sta.ssid, command.ssid, strlen(command.ssid));
    memcpy(config_.sta.password, command.password, strlen(command.password));
    config_.sta.pmf_cfg.capable = true;
    config_.sta.pmf_cfg.required = false;
    save_on_connect_ = true;
    retries_ = 0;
    {
        Lock lock(mutex_);
        strlcpy(status_.ssid, command.ssid, sizeof(status_.ssid));
        status_.connection_pending = false;
        disconnect_reason_ = 0;
    }
    setMessage(NetworkState::Connecting, "正在连接，成功后保存网络");
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &config_);
    if (err == ESP_OK) {
        want_connection_ = true;
        connect_deadline_ = esp_timer_get_time() + 20000000;
        err = esp_wifi_connect();
    }
    if (err != ESP_OK) {
        want_connection_ = false;
        connect_deadline_ = 0;
        setMessage(NetworkState::Error, "无法发起连接，请重新输入");
        ESP_LOGW(TAG, "Connect failed: %s", esp_err_to_name(err));
    }
}

void SystemService::scheduleRetry(int reason)
{
    connect_deadline_ = 0;
    if (!want_connection_ || retry_at_) return;
    const int seconds = std::min(30, 2 << std::min(retries_, 4));
    ++retries_;
    retry_at_ = esp_timer_get_time() + static_cast<int64_t>(seconds) * 1000000;
    char message[128];
    const bool auth_error = reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || reason == WIFI_REASON_HANDSHAKE_TIMEOUT;
    if (auth_error) snprintf(message, sizeof(message), "认证失败，请检查密码；%d 秒后重试", seconds);
    else snprintf(message, sizeof(message), "网络断开（%d）；%d 秒后重试", reason, seconds);
    setMessage(auth_error ? NetworkState::Error : NetworkState::Retry, message);
    ESP_LOGW(TAG, "Disconnected reason=%d; retry in %ds", reason, seconds);
}

void SystemService::reconcileNetwork()
{
    esp_netif_ip_info_t info = {};
    wifi_ap_record_t ap = {};
    const bool connected = want_connection_ && esp_netif_is_netif_up(netif_) &&
        esp_netif_get_ip_info(netif_, &info) == ESP_OK && info.ip.addr != 0 &&
        esp_wifi_sta_get_ap_info(&ap) == ESP_OK &&
        strncmp(reinterpret_cast<const char *>(ap.ssid), reinterpret_cast<const char *>(config_.sta.ssid), 32) == 0;
    if (connected) {
        retry_at_ = connect_deadline_ = 0;
        retries_ = 0;
        esp_err_t save_error = ESP_OK;
        if (save_on_connect_) {
            save_error = persistCredentials();
            save_on_connect_ = save_error != ESP_OK;
        }
        if (!sntp_initialized_) {
            esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2, ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "pool.ntp.org"));
            const esp_err_t err = esp_netif_sntp_init(&sntp);
            sntp_initialized_ = err == ESP_OK;
            if (err != ESP_OK) ESP_LOGW(TAG, "SNTP: %s", esp_err_to_name(err));
        }
        Lock lock(mutex_);
        status_.state = NetworkState::Connected;
        status_.rssi = ap.rssi;
        disconnect_reason_ = 0;
        snprintf(status_.ip, sizeof(status_.ip), IPSTR, IP2STR(&info.ip));
        strlcpy(status_.message, save_error == ESP_OK ? "已连接，所有应用共享网络" : "已连接，但网络保存失败", sizeof(status_.message));
    } else if (want_connection_) {
        int reason;
        { Lock lock(mutex_); reason = disconnect_reason_; }
        // STA_START can arrive during an active connection; only disconnect events schedule retry.
        if (snapshot().state == NetworkState::Connected) scheduleRetry(reason);
        else if (reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || reason == WIFI_REASON_HANDSHAKE_TIMEOUT) {
            setMessage(NetworkState::Error, "认证失败，请检查密码；稍后重试");
        }
    }
}

esp_err_t SystemService::persistCredentials()
{
    nvs_handle_t storage;
    esp_err_t err = nvs_open(STORAGE, NVS_READWRITE, &storage);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(storage, "wifi", &config_, sizeof(config_));
    if (err == ESP_OK) err = nvs_commit(storage);
    nvs_close(storage);
    return err;
}

esp_err_t SystemService::persistBrightness(int value)
{
    nvs_handle_t storage;
    esp_err_t err = nvs_open(STORAGE, NVS_READWRITE, &storage);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(storage, "brightness", value);
    if (err == ESP_OK) err = nvs_commit(storage);
    nvs_close(storage);
    return err;
}

void SystemService::doForget()
{
    want_connection_ = false;
    save_on_connect_ = false;
    retry_at_ = connect_deadline_ = 0;
    esp_wifi_disconnect();
    memset(&config_, 0, sizeof(config_));
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &config_);
    nvs_handle_t storage;
    const esp_err_t open_error = nvs_open(STORAGE, NVS_READWRITE, &storage);
    if (open_error == ESP_OK) {
        esp_err_t erase_error = nvs_erase_key(storage, "wifi");
        if (erase_error == ESP_ERR_NVS_NOT_FOUND) erase_error = ESP_OK;
        if (erase_error == ESP_OK) erase_error = nvs_commit(storage);
        nvs_close(storage);
        if (err == ESP_OK) err = erase_error;
    } else if (err == ESP_OK) err = open_error;
    setMessage(NetworkState::Offline, err == ESP_OK ? "网络已忘记，请重新配网" : "网络删除失败，请重试");
    Lock lock(mutex_);
    status_.connection_pending = false;
    status_.ssid[0] = 0;
}

void SystemService::doTest()
{
    const int64_t started = esp_timer_get_time();
    char result[sizeof(status_.test_result)] = {};
    if (!timeValid()) {
        strlcpy(result, "已连接 Wi-Fi，等待校时\n时间有效后再测试 HTTPS", sizeof(result));
    } else {
        addrinfo hints = {}, *resolved = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        const int dns_error = getaddrinfo("www.espressif.com", "443", &hints, &resolved);
        if (resolved) freeaddrinfo(resolved);
        if (dns_error != 0) {
            snprintf(result, sizeof(result), "DNS 解析失败：%d\n无法解析 www.espressif.com\n检查路由器 DNS 后重试", dns_error);
            ESP_LOGW(TAG, "HTTPS probe DNS failed: %d", dns_error);
        } else {
            esp_http_client_config_t config = {};
            config.url = "https://www.espressif.com/robots.txt";
            config.timeout_ms = 12000;
            config.buffer_size = 512;
            config.buffer_size_tx = 512;
            config.crt_bundle_attach = esp_crt_bundle_attach;
            config.disable_auto_redirect = true;
            // Read only the authenticated response headers, avoiding a large web body.
            auto client = esp_http_client_init(&config);
            esp_err_t err = client ? esp_http_client_open(client, 0) : ESP_ERR_NO_MEM;
            if (err == ESP_OK && esp_http_client_fetch_headers(client) < 0) err = ESP_ERR_HTTP_FETCH_HEADER;
            const int code = client ? esp_http_client_get_status_code(client) : 0;
            int tls_error = 0, tls_flags = 0;
            const int socket_error = client ? esp_http_client_get_errno(client) : 0;
            const esp_err_t tls_result = client ? esp_http_client_get_and_clear_last_tls_error(client, &tls_error, &tls_flags) : ESP_OK;
            const long long elapsed = (esp_timer_get_time() - started) / 1000;
            ESP_LOGI(TAG, "HTTPS probe: result=%s HTTP=%d elapsed=%lldms TLS=0x%x mbedTLS=%d flags=0x%x socket=%d internal=%u PSRAM=%u",
                     esp_err_to_name(err), code, elapsed, tls_result, tls_error, tls_flags, socket_error,
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
            if (err == ESP_OK && code > 0) {
                snprintf(result, sizeof(result), "HTTPS 已连通\nHTTP 响应：%d\n证书验证通过 · %lld ms\nwww.espressif.com", code, elapsed);
            } else {
                const char *hint = err == ESP_ERR_NO_MEM ? "内存不足" : tls_flags ? "证书校验失败" : "目标网站连接失败，可重试";
                snprintf(result, sizeof(result), "DNS 已解析，时间已同步\nHTTPS：%s\n%s\nTLS：0x%X / %d\nSocket：%d · %lld ms",
                         esp_err_to_name(err), hint, tls_result, tls_error, socket_error, elapsed);
            }
            if (client) esp_http_client_cleanup(client);
        }
    }
    Lock lock(mutex_);
    strlcpy(status_.test_result, result, sizeof(status_.test_result));
    status_.testing = false;
}
