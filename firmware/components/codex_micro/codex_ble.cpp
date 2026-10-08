// SPDX-License-Identifier: MIT
// Copyright (c) 2026 imliubo and Codex Micro port contributors
#include "codex_ble.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/task.h"
extern "C" {
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "host/ble_store.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
void ble_store_config_init(void);
}

namespace {
constexpr char kTag[] = "hid";
constexpr char kFirmwareVersion[] = "0.3.2";
constexpr size_t kReportBodySize = 63, kPayloadSize = 61;
CodexMicroBle *g_instance;
std::atomic<uint16_t> g_connection{BLE_HS_CONN_HANDLE_NONE};
std::atomic<uint32_t> g_session{0};
std::atomic<bool> g_synced{false}, g_inputSubscribed{false}, g_batterySubscribed{false}, g_encrypted{false};
uint8_t g_addressType;
uint16_t g_inputHandle, g_batteryHandle;
uint8_t s_inputReport[63] = {}, s_outputReport[63] = {};
std::atomic<uint8_t> s_batteryLevel{100};
portMUX_TYPE g_reportMux = portMUX_INITIALIZER_UNLOCKED;
uint8_t s_protocolMode = 1;
const uint8_t s_reportMap[] = {
  0x06,0x00,0xff,0x09,0x01,0xa1,0x01,0x85,0x06,0x15,0x00,0x26,0xff,0x00,
  0x75,0x08,0x95,0x3f,0x09,0x01,0x81,0x02,0x95,0x3f,0x09,0x02,0x91,0x02,0xc0
};
const uint8_t s_pnpId[] = {2,0x3a,0x30,0x60,0x83,1,1};
const uint8_t s_hidInfo[] = {0x11,1,0,1};
const uint8_t s_inputRef[] = {6,1}, s_outputRef[] = {6,2};
enum Field { Manufacturer, Pnp, HidInfo, ReportMap, Protocol, Input, Output, Control, Battery, InputRef, OutputRef };
const ble_uuid16_t uuids[] = {
  BLE_UUID16_INIT(0x2a29), BLE_UUID16_INIT(0x2a50), BLE_UUID16_INIT(0x2a4a),
  BLE_UUID16_INIT(0x2a4b), BLE_UUID16_INIT(0x2a4e), BLE_UUID16_INIT(0x2a4d),
  BLE_UUID16_INIT(0x2a4d), BLE_UUID16_INIT(0x2a4c), BLE_UUID16_INIT(0x2a19),
  BLE_UUID16_INIT(0x2908), BLE_UUID16_INIT(0x2908)
};
const ble_uuid16_t serviceUuids[] = {BLE_UUID16_INIT(0x180a), BLE_UUID16_INIT(0x1812), BLE_UUID16_INIT(0x180f)};
ble_gatt_chr_def disChars[3] = {}, hidChars[7] = {}, batteryChars[2] = {};
ble_gatt_dsc_def inputDescriptors[2] = {}, outputDescriptors[2] = {};
ble_gatt_svc_def services[4] = {};

struct Connections { uint16_t count = 0; uint16_t ids[1] = {}; };
Connections copyConnections() {
  Connections c;
  const auto id = g_connection.load();
  if (id != BLE_HS_CONN_HANDLE_NONE) { c.count = 1; c.ids[0] = id; }
  return c;
}
int access(uint16_t conn, uint16_t, ble_gatt_access_ctxt *ctx, void *arg) {
  const auto field = static_cast<Field>(reinterpret_cast<intptr_t>(arg));
  if (ctx->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
    uint8_t bytes[64]; uint16_t length = 0;
    const int rc = ble_hs_mbuf_to_flat(ctx->om, bytes, sizeof(bytes), &length);
    if (rc || !length) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (field == Output) {
      if (!g_instance->onOutput(bytes, length, conn)) return BLE_ATT_ERR_INSUFFICIENT_RES;
      memcpy(s_outputReport, bytes, length < sizeof(s_outputReport) ? length : sizeof(s_outputReport));
    } else if (field == Protocol) {
      if (length != 1 || bytes[0] != 1) return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
      s_protocolMode = bytes[0];
    } else if (field != Control || length != 1) return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    return 0;
  }
  const void *data = nullptr; size_t size = 0;
  uint8_t inputCopy[63], batteryCopy = s_batteryLevel.load();
  switch (field) {
    case Manufacturer: data = "OpenAI"; size = 6; break;
    case Pnp: data = s_pnpId; size = sizeof(s_pnpId); break;
    case HidInfo: data = s_hidInfo; size = sizeof(s_hidInfo); break;
    case ReportMap: data = s_reportMap; size = sizeof(s_reportMap); break;
    case Protocol: data = &s_protocolMode; size = 1; break;
    case Input:
      portENTER_CRITICAL(&g_reportMux); memcpy(inputCopy, s_inputReport, sizeof(inputCopy)); portEXIT_CRITICAL(&g_reportMux);
      data = inputCopy; size = sizeof(inputCopy); break;
    case Output: data = s_outputReport; size = sizeof(s_outputReport); break;
    case Battery: data = &batteryCopy; size = 1; break;
    case InputRef: data = s_inputRef; size = sizeof(s_inputRef); break;
    case OutputRef: data = s_outputRef; size = sizeof(s_outputRef); break;
    default: return BLE_ATT_ERR_READ_NOT_PERMITTED;
  }
  return os_mbuf_append(ctx->om, data, size) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
}
void defineCharacteristic(ble_gatt_chr_def &chr, Field field, uint16_t flags, uint16_t *handle = nullptr) {
  chr.uuid = &uuids[field].u; chr.access_cb = access; chr.arg = reinterpret_cast<void *>(static_cast<intptr_t>(field));
  chr.flags = flags; chr.val_handle = handle;
}
void defineServices() {
  const uint16_t read = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC;
  const uint16_t write = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC;
  defineCharacteristic(disChars[0], Manufacturer, BLE_GATT_CHR_F_READ);
  defineCharacteristic(disChars[1], Pnp, BLE_GATT_CHR_F_READ);
  defineCharacteristic(hidChars[0], HidInfo, read);
  defineCharacteristic(hidChars[1], ReportMap, read);
  defineCharacteristic(hidChars[2], Protocol, read | write | BLE_GATT_CHR_F_WRITE_NO_RSP);
  defineCharacteristic(hidChars[3], Input, read | BLE_GATT_CHR_F_NOTIFY, &g_inputHandle);
  defineCharacteristic(hidChars[4], Output, read | write | BLE_GATT_CHR_F_WRITE_NO_RSP);
  defineCharacteristic(hidChars[5], Control, write | BLE_GATT_CHR_F_WRITE_NO_RSP);
  inputDescriptors[0].uuid = &uuids[InputRef].u;
  inputDescriptors[0].att_flags = BLE_ATT_F_READ;
  inputDescriptors[0].access_cb = access;
  inputDescriptors[0].arg = reinterpret_cast<void *>(InputRef);
  outputDescriptors[0] = inputDescriptors[0];
  outputDescriptors[0].uuid = &uuids[OutputRef].u;
  outputDescriptors[0].arg = reinterpret_cast<void *>(OutputRef);
  hidChars[3].descriptors = inputDescriptors;
  hidChars[4].descriptors = outputDescriptors;
  defineCharacteristic(batteryChars[0], Battery, BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, &g_batteryHandle);
  ble_gatt_chr_def *tables[] = {disChars, hidChars, batteryChars};
  for (int i = 0; i < 3; ++i) {
    services[i].type = BLE_GATT_SVC_TYPE_PRIMARY;
    services[i].uuid = &serviceUuids[i].u; services[i].characteristics = tables[i];
  }
}
int gapEvent(ble_gap_event *event, void *);
void startAdvertising() {
  if (!g_synced || !g_instance->enabled() || g_connection != BLE_HS_CONN_HANDLE_NONE) return;
  if (ble_gap_adv_active()) { g_instance->onAdvertisingState(true); return; }
  ble_hs_adv_fields fields{};
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  const ble_uuid16_t hid = BLE_UUID16_INIT(0x1812);
  fields.uuids16 = const_cast<ble_uuid16_t *>(&hid); fields.num_uuids16 = 1; fields.uuids16_is_complete = 1;
  const char *name = "Codex Micro";
  fields.name = reinterpret_cast<const uint8_t *>(name); fields.name_len = strlen(name); fields.name_is_complete = 1;
  fields.appearance = 0x03c0; fields.appearance_is_present = 1;
  int rc = ble_gap_adv_set_fields(&fields);
  ble_gap_adv_params params{};
  params.conn_mode = BLE_GAP_CONN_MODE_UND; params.disc_mode = BLE_GAP_DISC_MODE_GEN;
  if (!rc) rc = ble_gap_adv_start(g_addressType, nullptr, BLE_HS_FOREVER, &params, gapEvent, nullptr);
  g_instance->onAdvertisingState(rc == 0 || ble_gap_adv_active());
  if (rc) ESP_LOGW(kTag, "advertising failed rc=%d", rc);
  else ESP_LOGI(kTag, "NimBLE advertising as Codex Micro");
}
int gapEvent(ble_gap_event *event, void *) {
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      g_instance->onAdvertisingState(false);
      if (event->connect.status) { startAdvertising(); break; }
      g_connection = event->connect.conn_handle;
      ++g_session;
      g_inputSubscribed = g_batterySubscribed = g_encrypted = false;
      g_instance->onConnectionEvent(true, event->connect.conn_handle);
      ESP_LOGI(kTag, "connected id=%u", event->connect.conn_handle);
      // HOGP requires encryption; Just Works preserves the desktop HID UX.
      ble_gap_security_initiate(event->connect.conn_handle);
      break;
    case BLE_GAP_EVENT_DISCONNECT:
      g_connection = BLE_HS_CONN_HANDLE_NONE;
      ++g_session;
      g_inputSubscribed = g_batterySubscribed = g_encrypted = false;
      g_instance->onConnectionEvent(false, event->disconnect.conn.conn_handle);
      ESP_LOGW(kTag, "disconnected id=%u reason=0x%x", event->disconnect.conn.conn_handle, event->disconnect.reason);
      startAdvertising();
      break;
    case BLE_GAP_EVENT_SUBSCRIBE:
      if (event->subscribe.attr_handle == g_inputHandle) g_inputSubscribed = event->subscribe.cur_notify;
      if (event->subscribe.attr_handle == g_batteryHandle) g_batterySubscribed = event->subscribe.cur_notify;
      ESP_LOGI(kTag, "subscription handle=%u notify=%u", event->subscribe.attr_handle, event->subscribe.cur_notify);
      break;
    case BLE_GAP_EVENT_ENC_CHANGE: {
      ble_gap_conn_desc desc{};
      if (!event->enc_change.status && !ble_gap_conn_find(event->enc_change.conn_handle, &desc)) g_encrypted = desc.sec_state.encrypted;
      ESP_LOGI(kTag, "encryption status=%d encrypted=%d", event->enc_change.status, g_encrypted.load());
      break;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
      ble_gap_conn_desc desc{};
      if (!ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc)) ble_store_util_delete_peer(&desc.peer_id_addr);
      return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_CONN_UPDATE: {
      ble_gap_conn_desc desc{};
      if (!ble_gap_conn_find(event->conn_update.conn_handle, &desc))
        ESP_LOGI(kTag, "link interval=%u latency=%u timeout=%u status=%d", desc.conn_itvl, desc.conn_latency, desc.supervision_timeout, event->conn_update.status);
      break;
    }
    case BLE_GAP_EVENT_MTU: ESP_LOGI(kTag, "MTU=%u", event->mtu.value); break;
    case BLE_GAP_EVENT_ADV_COMPLETE: g_instance->onAdvertisingState(false); startAdvertising(); break;
    default: break;
  }
  return 0;
}
void onSync() {
  const int rc = ble_hs_util_ensure_addr(0);
  if (rc || ble_hs_id_infer_auto(0, &g_addressType)) { ESP_LOGE(kTag, "address initialization failed"); return; }
  g_synced = true; startAdvertising();
}
void onReset(int reason) {
  g_synced = false;
  const auto conn = g_connection.exchange(BLE_HS_CONN_HANDLE_NONE);
  ++g_session;
  g_instance->onAdvertisingState(false);
  if (conn != BLE_HS_CONN_HANDLE_NONE) g_instance->onConnectionEvent(false, conn);
  ESP_LOGE(kTag, "host reset reason=%d", reason);
}
void hostTask(void *) { nimble_port_run(); nimble_port_freertos_deinit(); }
}

esp_err_t CodexMicroBle::begin(bool enabled) {
  g_instance = this; enabled_ = enabled;
  stateMutex_ = xSemaphoreCreateMutex();
  outputQueue_ = xQueueCreate(16, sizeof(PendingOutputReport));
  connectionEventQueue_ = xQueueCreate(16, sizeof(PendingConnectionEvent));
  if (!stateMutex_ || !outputQueue_ || !connectionEventQueue_) return ESP_ERR_NO_MEM;
  const esp_err_t error = nimble_port_init();
  if (error != ESP_OK) return error;
  ble_hs_cfg.sync_cb = onSync; ble_hs_cfg.reset_cb = onReset;
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
  ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
  ble_hs_cfg.sm_bonding = 1; ble_hs_cfg.sm_mitm = 0; ble_hs_cfg.sm_sc = 1;
  ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_store_config_init();
  ble_svc_gap_init(); ble_svc_gatt_init();
  ble_svc_gap_device_name_set("Codex Micro");
  ble_att_set_preferred_mtu(517);
  defineServices();
  int rc = ble_gatts_count_cfg(services);
  if (!rc) rc = ble_gatts_add_svcs(services);
  if (rc) { ESP_LOGE(kTag, "GATT initialization failed rc=%d", rc); return ESP_FAIL; }
  nimble_port_freertos_init(hostTask);
  ESP_LOGI(kTag, "NimBLE HOGP VID=303A PID=8360 report=6");
  return ESP_OK;
}
esp_err_t CodexMicroBle::setEnabled(bool enabled) {
  enabled_ = enabled;
  if (!g_synced) return ESP_OK;
  if (enabled) startAdvertising();
  else {
    if (ble_gap_adv_active()) ble_gap_adv_stop();
    onAdvertisingState(false);
    const auto conn = g_connection.load();
    if (conn != BLE_HS_CONN_HANDLE_NONE) {
      const int rc = ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
      if (rc && rc != BLE_HS_ENOTCONN) return ESP_FAIL;
    }
  }
  return ESP_OK;
}
int CodexMicroBle::bondCount() const {
  int count = 0;
  if (g_synced) ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &count);
  return count;
}
int CodexMicroBle::enterPairingMode() {
  if (!g_synced || g_connection != BLE_HS_CONN_HANDLE_NONE) return -1;
  const int count = bondCount();
  if (ble_store_clear()) return -1;
  rpcLength_ = 0; rpcBufferConnectionValid_ = false;
  ESP_LOGI(kTag, "forgot %d NimBLE bonds", count);
  return count;
}
void CodexMicroBle::poll() {
  processConnectionEvents(); reconcileConnectionSet();
  PendingOutputReport report;
  while (xQueueReceive(outputQueue_, &report, 0) == pdTRUE) {
    if (!report.length) { rpcLength_ = 0; continue; }
    // The controller can reuse a connection ID immediately after reconnect.
    if (report.session == g_session) processOutput(report.data.data(), report.length, report.connectionId);
  }
  startAdvertising();
}
void CodexMicroBle::setBattery(uint8_t percentage, bool charging) {
  batteryPercentage_ = percentage > 100 ? 100 : percentage; charging_ = charging;
  const bool changed = s_batteryLevel != batteryPercentage_;
  s_batteryLevel = batteryPercentage_;
  if (changed && g_batterySubscribed && g_connection != BLE_HS_CONN_HANDLE_NONE) {
    const uint8_t value = batteryPercentage_;
    auto *buffer = ble_hs_mbuf_from_flat(&value, 1);
    if (buffer) ble_gatts_notify_custom(g_connection, g_batteryHandle, buffer);
  }
}
bool CodexMicroBle::onOutput(const uint8_t *data, size_t length, uint16_t connectionId) {
  if (!data || !length || length > 64 || !outputQueue_) return false;
  PendingOutputReport report; report.length = length; report.connectionId = connectionId; report.session = g_session;
  memcpy(report.data.data(), data, length);
  return xQueueSend(outputQueue_, &report, 0) == pdTRUE;
}
void CodexMicroBle::sendJson(const char *json) {
  if (!enabled() || !json || !g_inputSubscribed || !g_encrypted || g_connection == BLE_HS_CONN_HANDLE_NONE) return;
  const size_t length = strlen(json);
  if (length + 1 > 2048) { ESP_LOGW(kTag, "HID message too large"); return; }
  char framed[2048]; memcpy(framed, json, length); framed[length] = '\n';
  const uint16_t conn = g_connection;
  for (size_t offset = 0; offset < length + 1;) {
    const size_t chunk = std::min(kPayloadSize, length + 1 - offset);
    uint8_t report[kReportBodySize] = {}; report[0] = 2; report[1] = chunk;
    memcpy(report + 2, framed + offset, chunk);
    portENTER_CRITICAL(&g_reportMux); memcpy(s_inputReport, report, sizeof(report)); portEXIT_CRITICAL(&g_reportMux);
    int rc = BLE_HS_ENOMEM;
    for (int retry = 0; retry < 3 && rc == BLE_HS_ENOMEM; ++retry) {
      auto *mbuf = ble_hs_mbuf_from_flat(report, sizeof(report));
      if (mbuf) rc = ble_gatts_notify_custom(conn, g_inputHandle, mbuf);
      if (rc == BLE_HS_ENOMEM) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (rc) { ESP_LOGW(kTag, "HID notify failed rc=%d", rc); break; }
    offset += chunk;
    vTaskDelay(pdMS_TO_TICKS(4));
  }
}
void CodexMicroBle::sendKey(const char* key, uint8_t action, int8_t agent) {
  cJSON* message = cJSON_CreateObject();
  if (message == nullptr) return;
  cJSON_AddStringToObject(message, "method", "v.oai.hid");
  cJSON* params = cJSON_AddObjectToObject(message, "params");
  cJSON_AddStringToObject(params, "k", key);
  cJSON_AddNumberToObject(params, "act", action);
  if (agent >= 0) cJSON_AddNumberToObject(params, "ag", agent);

  char* json = cJSON_PrintUnformatted(message);
  if (json != nullptr) {
    sendJson(json);
    cJSON_free(json);
  }
  cJSON_Delete(message);
  ESP_LOGI(kTag, "HID key=%s action=%u", key, action);
}

void CodexMicroBle::sendJoystick(float angle, float distance) {
  cJSON* message = cJSON_CreateObject();
  if (message == nullptr) return;
  cJSON_AddStringToObject(message, "method", "v.oai.rad");
  cJSON* params = cJSON_AddObjectToObject(message, "params");
  cJSON_AddNumberToObject(params, "a", angle);
  cJSON_AddNumberToObject(params, "d", distance);

  char* json = cJSON_PrintUnformatted(message);
  if (json != nullptr) {
    sendJson(json);
    cJSON_free(json);
  }
  cJSON_Delete(message);
}

bool CodexMicroBle::connected() {
  if (stateMutex_ == nullptr) return false;
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  const bool result = state_.connected;
  xSemaphoreGive(stateMutex_);
  return result;
}

CodexMicroState CodexMicroBle::snapshot() {
  CodexMicroState copy;
  if (stateMutex_ == nullptr) return copy;
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  copy = state_;
  state_.dirty = false;
  xSemaphoreGive(stateMutex_);
  return copy;
}

void CodexMicroBle::onConnectionEvent(bool connected, uint16_t id) {
  if (connectionEventQueue_ == nullptr) {
    connectionEventLost_.store(true, std::memory_order_release);
    return;
  }
  PendingConnectionEvent event;
  event.id = id;
  event.connected = connected;
  if (xQueueSend(connectionEventQueue_, &event, 0) != pdTRUE) {
    connectionEventLost_.store(true, std::memory_order_release);
  }
}

void CodexMicroBle::applyConnectionEvent(const PendingConnectionEvent& event) {
  const connection_health::ConnectionSet::Transition transition =
      connections_.apply(event.connected, event.id);
  if (transition.overflow) {
    connectionEventLost_.store(true, std::memory_order_release);
    return;
  }
  if (!transition.changed || stateMutex_ == nullptr) return;

  const uint8_t count = static_cast<uint8_t>(connections_.count());

  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  const bool hostDisconnected = !event.connected && hostRpcConnectionValid_ &&
                                hostRpcConnectionId_ == event.id;
  if (transition.becameConnected) {
    ++state_.connectionEpoch;
    state_.lastHostRpcAtMs = 0;
    state_.hostRpcObserved = false;
    clearHostRpcIdentity();
  } else if (transition.becameDisconnected || hostDisconnected) {
    state_.lastHostRpcAtMs = 0;
    state_.hostRpcObserved = false;
    clearHostRpcIdentity();
  }
  state_.connected = count > 0;
  state_.dirty = true;
  const uint32_t connectionEpoch = state_.connectionEpoch;
  xSemaphoreGive(stateMutex_);

  if (transition.becameConnected || transition.becameDisconnected ||
      (!event.connected && rpcBufferConnectionValid_ &&
       rpcBufferConnectionId_ == event.id)) {
    rpcLength_ = 0;
    rpcBufferConnectionValid_ = false;
  }
  ESP_LOGI(kTag, "host event=%s id=%u count=%u epoch=%lu",
           event.connected ? "connected" : "disconnected", event.id, count,
           static_cast<unsigned long>(connectionEpoch));
}

void CodexMicroBle::processConnectionEvents() {
  if (connectionEventQueue_ == nullptr) return;
  PendingConnectionEvent event;
  while (xQueueReceive(connectionEventQueue_, &event, 0) == pdTRUE) {
    applyConnectionEvent(event);
  }
}

void CodexMicroBle::reconcileConnectionSet() {
  const auto peers = copyConnections();
  const bool eventLost =
      connectionEventLost_.exchange(false, std::memory_order_acq_rel);
  if (!eventLost || stateMutex_ == nullptr) return;

  // At least one ordered event was lost. Fail closed even if final membership
  // happens to match: an unseen empty->connected cycle must never inherit the
  // old session's CODEX LIVE state.
  connection_health::ConnectionSet observed;
  bool overflow = false;
  for (uint16_t i = 0; i < peers.count; ++i) {
    if (observed.apply(true, peers.ids[i]).overflow) overflow = true;
  }
  if (overflow) {
    connectionEventLost_.store(true, std::memory_order_release);
    return;
  }

  connections_ = observed;
  const uint8_t count = static_cast<uint8_t>(connections_.count());
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  if (count > 0) ++state_.connectionEpoch;
  state_.connected = count > 0;
  state_.lastHostRpcAtMs = 0;
  state_.hostRpcObserved = false;
  clearHostRpcIdentity();
  state_.dirty = true;
  const uint32_t epoch = state_.connectionEpoch;
  xSemaphoreGive(stateMutex_);
  rpcLength_ = 0;
  rpcBufferConnectionValid_ = false;
  ESP_LOGI(kTag, "connections reconciled count=%u epoch=%lu", count,
           static_cast<unsigned long>(epoch));
}

void CodexMicroBle::clearHostRpcIdentity() {
  hostRpcConnectionValid_ = false;
}

void CodexMicroBle::noteHostRpcActivity(uint16_t connectionId) {
  if (stateMutex_ == nullptr || !connections_.contains(connectionId)) return;
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  if (!state_.connected) {
    xSemaphoreGive(stateMutex_);
    return;
  }
  const bool promoted = !state_.hostRpcObserved;
  state_.hostRpcObserved = true;
  state_.lastHostRpcAtMs = esp_log_timestamp();
  hostRpcConnectionId_ = connectionId;
  hostRpcConnectionValid_ = true;
  if (promoted) state_.dirty = true;
  xSemaphoreGive(stateMutex_);
}

namespace {

// Returns the number of unbalanced '{' minus '}' characters, skipping string
// literals and escapes. Zero means the buffer holds a complete JSON value.
int jsonBraceDepth(const char* text, size_t length) {
  int depth = 0;
  bool inString = false;
  bool escaped = false;
  for (size_t i = 0; i < length; ++i) {
    const char c = text[i];
    if (escaped) {
      escaped = false;
      continue;
    }
    if (c == '\\') {
      escaped = true;
      continue;
    }
    if (c == '"') {
      inString = !inString;
      continue;
    }
    if (inString) continue;
    if (c == '{') ++depth;
    if (c == '}') --depth;
  }
  return depth;
}

}  // namespace

void CodexMicroBle::processOutput(const uint8_t* data, size_t length,
                                  uint16_t connectionId) {
  if (data == nullptr || length < 2 || !connections_.contains(connectionId)) {
    return;
  }

  if (rpcBufferConnectionValid_ && rpcBufferConnectionId_ != connectionId) {
    rpcLength_ = 0;
    rpcBufferConnectionValid_ = false;
  }

  // HOGP normally strips the report ID. Accept an included ID as well so the
  // transport remains compatible with hosts that forward the raw report.
  size_t offset = (length >= 3 && data[0] == kReportId) ? 1 : 0;
  if (length < offset + 2 || data[offset] != 2) return;

  const size_t payloadLength =
      data[offset + 1] < kPayloadSize ? data[offset + 1] : kPayloadSize;
  if (length < offset + 2 + payloadLength) return;
  const char* payload = reinterpret_cast<const char*>(data + offset + 2);

  constexpr char kTopLevelPrefix[] = "{\"method\"";
  const bool startsTopLevel =
      payloadLength >= sizeof(kTopLevelPrefix) - 1 &&
      memcmp(payload, kTopLevelPrefix, sizeof(kTopLevelPrefix) - 1) == 0;
  if (startsTopLevel && rpcLength_ > 0) {
    // A new top-level object means a previous fragmented write was dropped.
    // Resynchronize immediately instead of poisoning the next request.
    rpcLength_ = 0;
  }

  if (rpcLength_ == 0) {
    size_t jsonStart = 0;
    while (jsonStart < payloadLength && payload[jsonStart] != '{') ++jsonStart;
    if (jsonStart == payloadLength) return;
    rpcLength_ = payloadLength - jsonStart;
    if (rpcLength_ >= sizeof(rpcBuffer_)) rpcLength_ = sizeof(rpcBuffer_) - 1;
    memcpy(rpcBuffer_, payload + jsonStart, rpcLength_);
    rpcBufferConnectionId_ = connectionId;
    rpcBufferConnectionValid_ = true;
  } else {
    size_t room = sizeof(rpcBuffer_) - 1 - rpcLength_;
    size_t chunk = payloadLength < room ? payloadLength : room;
    memcpy(rpcBuffer_ + rpcLength_, payload, chunk);
    rpcLength_ += chunk;
  }
  rpcBuffer_[rpcLength_] = '\0';

  if (jsonBraceDepth(rpcBuffer_, rpcLength_) > 0) return;  // still incomplete

  cJSON* request = cJSON_ParseWithLength(rpcBuffer_, rpcLength_);
  if (request == nullptr) {
    ESP_LOGW(kTag, "RPC parse error: %s", rpcBuffer_);
    rpcLength_ = 0;
    rpcBufferConnectionValid_ = false;
    return;
  }

  if (handleRpc(request)) {
    noteHostRpcActivity(connectionId);
  }
  cJSON_Delete(request);
  rpcLength_ = 0;
  rpcBufferConnectionValid_ = false;
}

bool CodexMicroBle::handleRpc(const cJSON* request) {
  const cJSON* methodItem =
      cJSON_GetObjectItemCaseSensitive(request, "method");
  const char* method =
      (cJSON_IsString(methodItem) && methodItem->valuestring != nullptr)
          ? methodItem->valuestring
          : "";
  const cJSON* id = cJSON_GetObjectItemCaseSensitive(request, "id");
  const cJSON* params = cJSON_GetObjectItemCaseSensitive(request, "params");
  const host_rpc::Method supportedMethod = host_rpc::classify(request);
  ESP_LOGI(kTag, "RPC method=%s", method);

  if (supportedMethod == host_rpc::Method::SystemVersion) {
    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "version", kFirmwareVersion);
    sendResult(id, result);
    return true;
  }

  if (supportedMethod == host_rpc::Method::DeviceStatus) {
    cJSON* result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "version", kFirmwareVersion);
    cJSON_AddNumberToObject(result, "profile_index", 0);
    cJSON_AddNumberToObject(result, "layer_index", 1);
    cJSON_AddNumberToObject(result, "battery", batteryPercentage_);
    cJSON_AddBoolToObject(result, "is_charging", charging_);
    sendResult(id, result);
    return true;
  }

  if (supportedMethod == host_rpc::Method::ThreadStatus) {
    updateThreadLighting(params);
    sendSuccess(id);
    return true;
  }

  if (supportedMethod == host_rpc::Method::RgbConfig ||
      supportedMethod == host_rpc::Method::LightsPreview ||
      supportedMethod == host_rpc::Method::HostFocusedApp) {
    sendSuccess(id);
    return true;
  }

  cJSON* response = cJSON_CreateObject();
  cJSON_AddItemToObject(response, "id",
                        id != nullptr ? cJSON_Duplicate(id, true)
                                      : cJSON_CreateNull());
  cJSON* error = cJSON_AddObjectToObject(response, "error");
  cJSON_AddNumberToObject(error, "code", -32601);
  cJSON_AddStringToObject(error, "message", "Method not found");
  char* json = cJSON_PrintUnformatted(response);
  if (json != nullptr) {
    sendJson(json);
    cJSON_free(json);
  }
  cJSON_Delete(response);
  return false;
}

void CodexMicroBle::sendResult(const cJSON* id, cJSON* result) {
  cJSON* response = cJSON_CreateObject();
  cJSON_AddItemToObject(response, "id",
                        id != nullptr ? cJSON_Duplicate(id, true)
                                      : cJSON_CreateNull());
  if (result != nullptr) {
    cJSON_AddItemToObject(response, "result", result);
  } else {
    cJSON_AddNullToObject(response, "result");
  }
  char* json = cJSON_PrintUnformatted(response);
  if (json != nullptr) {
    sendJson(json);
    cJSON_free(json);
  }
  cJSON_Delete(response);
}

void CodexMicroBle::sendSuccess(const cJSON* id) {
  cJSON* result = cJSON_CreateObject();
  cJSON_AddBoolToObject(result, "ok", true);
  sendResult(id, result);
}

void CodexMicroBle::updateThreadLighting(const cJSON* values) {
  if (!cJSON_IsArray(values)) return;
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  const cJSON* value = nullptr;
  cJSON_ArrayForEach(value, values) {
    const cJSON* idItem = cJSON_GetObjectItemCaseSensitive(value, "id");
    if (!cJSON_IsNumber(idItem)) continue;
    const int id = idItem->valueint;
    if (id < 0 || id >= static_cast<int>(state_.threads.size())) continue;

    ThreadLight& light = state_.threads[id];
    const cJSON* color = cJSON_GetObjectItemCaseSensitive(value, "c");
    if (cJSON_IsNumber(color)) light.color = static_cast<uint32_t>(color->valuedouble);
    const cJSON* brightness = cJSON_GetObjectItemCaseSensitive(value, "b");
    if (cJSON_IsNumber(brightness)) {
      light.brightness = static_cast<float>(brightness->valuedouble);
    }
    const cJSON* effect = cJSON_GetObjectItemCaseSensitive(value, "e");
    if (cJSON_IsString(effect) && effect->valuestring != nullptr) {
      strncpy(light.effect, effect->valuestring, sizeof(light.effect) - 1);
      light.effect[sizeof(light.effect) - 1] = '\0';
    }
    const cJSON* speed = cJSON_GetObjectItemCaseSensitive(value, "s");
    if (cJSON_IsNumber(speed)) {
      light.speed = static_cast<float>(speed->valuedouble);
    }
  }
  state_.dirty = true;
  xSemaphoreGive(stateMutex_);
}
