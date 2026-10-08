// SPDX-License-Identifier: MIT
// Copyright (c) 2026 imliubo and Codex Micro port contributors
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "logic.h"
struct ThreadLight { uint32_t color = 0; float brightness = 0; char effect[12] = "off"; float speed = 0; };
struct CodexMicroState {
  std::array<ThreadLight, 6> threads;
  bool connected = false;
  bool hostRpcObserved = false;
  uint32_t lastHostRpcAtMs = 0;
  uint32_t connectionEpoch = 0;
  bool dirty = true;
};
// NimBLE HOGP transport. Quota belongs to the independent HTTP service.
class CodexMicroBle {
 public:
  static constexpr uint16_t kVendorId = 0x303A, kProductId = 0x8360;
  static constexpr uint8_t kReportId = 6;
  esp_err_t begin(bool enabled = true);
  esp_err_t setEnabled(bool enabled);
  bool enabled() const { return enabled_.load(); }
  bool advertising() const { return advertising_.load(); }
  bool connected();
  void poll();
  void setBattery(uint8_t percentage, bool charging);
  void sendKey(const char *key, uint8_t action, int8_t agent = -1);
  void sendJoystick(float angle, float distance);
  CodexMicroState snapshot();
  int enterPairingMode();
  int bondCount() const;
  void onConnectionEvent(bool connected, uint16_t id);
  void onAdvertisingState(bool advertising) { advertising_.store(advertising); }
  bool onOutput(const uint8_t *data, size_t length, uint16_t connectionId);
 private:
  struct PendingOutputReport { uint8_t length = 0; uint16_t connectionId = 0; uint32_t session = 0; std::array<uint8_t, 64> data = {}; };
  struct PendingConnectionEvent { uint16_t id = 0; bool connected = false; };
  void applyConnectionEvent(const PendingConnectionEvent &event);
  void processConnectionEvents();
  void processOutput(const uint8_t *data, size_t length, uint16_t connectionId);
  void reconcileConnectionSet();
  void clearHostRpcIdentity();
  void noteHostRpcActivity(uint16_t connectionId);
  bool handleRpc(const cJSON *request);
  void sendResult(const cJSON *id, cJSON *result);
  void sendSuccess(const cJSON *id);
  void sendJson(const char *json);
  void updateThreadLighting(const cJSON *values);
  SemaphoreHandle_t stateMutex_ = nullptr;
  QueueHandle_t outputQueue_ = nullptr, connectionEventQueue_ = nullptr;
  CodexMicroState state_;
  char rpcBuffer_[4096] = {};
  size_t rpcLength_ = 0;
  uint16_t rpcBufferConnectionId_ = 0, hostRpcConnectionId_ = 0;
  bool rpcBufferConnectionValid_ = false, hostRpcConnectionValid_ = false;
  uint8_t batteryPercentage_ = 100;
  bool charging_ = false;
  TickType_t lastAdvertisingCheck_ = 0;
  connection_health::ConnectionSet connections_;
  std::atomic<bool> connectionEventLost_{false}, advertising_{false}, enabled_{false};
};
