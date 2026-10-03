// SPDX-License-Identifier: MIT
// Copyright (c) 2026 imliubo
// StopWatch port changes copyright (c) 2026 Codex Micro for StopWatch contributors
// Waveshare ESP32-S3-Touch-LCD-1.85B port copyright (c) 2026 Codex Micro port
//
// Bluedroid emulation of the Codex Micro BLE protocol.
//
// The C152 firmware used the Arduino-ESP32 BLE wrapper. This board runs a
// native ESP-IDF project, so the same GATT layout is registered directly
// through the Bluedroid GATTS/GAP APIs:
//
//   * Device Information 0x180A  manufacturer + PnP ID (303A:8360)
//   * HID over GATT     0x1812  vendor report 6, usage page 0xFF00
//   * Battery Service   0x180F  battery level notifications
//   * Private quota     7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c01
//
// All control actions still belong to ChatGPT Desktop; this device only relays
// key/joystick intents and displays host-provided telemetry.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "logic.h"

struct ThreadLight {
  uint32_t color = 0;
  float brightness = 0.0f;
  char effect[12] = "off";
  float speed = 0.0f;
};

struct QuotaState {
  float fiveHourRemainingPercent = 0.0f;
  uint32_t fiveHourResetInSeconds = 0;
  float weeklyRemainingPercent = 0.0f;
  uint32_t weeklyResetInSeconds = 0;
  uint32_t receivedAtMs = 0;
  bool available = false;
  // True when the values above were loaded from NVS rather than received from a
  // companion in this power cycle. A restored snapshot has no usable countdown
  // (no RTC), so the dashboard must render it as stale until a live write
  // arrives and clears this flag.
  bool restored = false;
};

struct CodexMicroState {
  std::array<ThreadLight, 6> threads;
  QuotaState quota;
  bool connected = false;
  bool hostRpcObserved = false;
  uint32_t lastHostRpcAtMs = 0;
  uint32_t connectionEpoch = 0;
  bool dirty = true;
};

class CodexMicroBle {
 public:
  static constexpr uint16_t kVendorId = 0x303A;
  static constexpr uint16_t kProductId = 0x8360;
  static constexpr uint8_t kReportId = 6;

  esp_err_t begin(bool enabled = true);
  esp_err_t setEnabled(bool enabled);
  bool enabled() const { return enabled_.load(); }
  void poll();
  void setBattery(uint8_t percentage, bool charging);
  void sendKey(const char* key, uint8_t action, int8_t agent = -1);
  void sendJoystick(float angle, float distance);
  bool connected();
  bool advertising() const { return advertising_.load(); }
  CodexMicroState snapshot();

  // ---------------------------------------------------------- pairing mode --
  //
  // The button-driven equivalent of holding the pairing key on a headset:
  // drop the host link, throw away every stored bond, and go back to pairable
  // advertising so the host is forced to pair from scratch.
  //
  // This is the software cure for the failure the whole BLE section of the
  // README is about. A bond has two halves; when the host's half survives a
  // reflash, an NVS erase or an address change, the host keeps trying to
  // resume encryption with a key this board no longer holds, the link comes up
  // unencrypted, and the desktop app reports the device as "connected with
  // limited functionality". Dropping this board's half on demand is the one
  // recovery action that does not need a serial console, a reflash or Windows'
  // Settings app.
  //
  // Returns the number of bonds that were removed.
  int enterPairingMode();

  // The escalation for a host record that is stuck in the contradictory
  // "reports unpaired, refuses to pair" state: bump the stored bond generation
  // and reboot, so the board advertises an address the host has never seen.
  // The host then treats it as a brand-new device and pairs it cleanly.
  //
  // Only returns on failure -- success ends in esp_restart().
  bool resetBondGenerationAndRestart();

  // Exposed for the GAP/GATTS callbacks, which are plain C entry points.
  void onConnectionEvent(bool connected, uint16_t id);
  void onAdvertisingState(bool advertising) { advertising_.store(advertising); }
  void onOutput(const uint8_t* data, size_t length, uint16_t connectionId,
                const uint8_t* peerAddress);
  void onQuotaWrite(const uint8_t* data, size_t length, bool responseExpected,
                    const uint8_t* peerAddress);
  void setReportHandles(uint16_t input, uint16_t output, uint16_t battery,
                        uint16_t quota);
  void markDisconnected(uint16_t connId);

 private:
  using PeerAddress = std::array<uint8_t, 6>;

  struct PendingOutputReport {
    uint8_t length = 0;
    uint16_t connectionId = 0;
    PeerAddress peerAddress = {};
    std::array<uint8_t, 64> data = {};
  };

  struct PendingConnectionEvent {
    uint16_t id = 0;
    bool connected = false;
  };

  struct PendingQuotaWrite {
    uint16_t length = 0;
    bool responseExpected = false;
    PeerAddress peerAddress = {};
    std::array<uint8_t, 512> data = {};
  };

  void applyConnectionEvent(const PendingConnectionEvent& event);
  void processConnectionEvents();
  void processQuotaWrites();
  void processQuotaWrite(const uint8_t* data, size_t length,
                         bool responseExpected,
                         const PeerAddress& peerAddress);
  void processOutput(const uint8_t* data, size_t length, uint16_t connectionId,
                     const PeerAddress& peerAddress);
  void reconcileConnectionSet();
  void clearHostRpcIdentity();
  void noteHostRpcActivity(uint16_t connectionId);

  bool handleRpc(const cJSON* request);
  void sendResult(const cJSON* id, cJSON* result);
  void sendSuccess(const cJSON* id);
  void sendJson(const char* json);
  void updateThreadLighting(const cJSON* values);

  SemaphoreHandle_t stateMutex_ = nullptr;
  QueueHandle_t outputQueue_ = nullptr;
  QueueHandle_t connectionEventQueue_ = nullptr;
  QueueHandle_t quotaWriteQueue_ = nullptr;

  CodexMicroState state_;
  char rpcBuffer_[4096] = {};
  size_t rpcLength_ = 0;
  uint16_t rpcBufferConnectionId_ = 0;
  bool rpcBufferConnectionValid_ = false;
  uint16_t hostRpcConnectionId_ = 0;
  bool hostRpcConnectionValid_ = false;
  uint8_t batteryPercentage_ = 100;
  bool charging_ = false;
  connection_health::ConnectionSet connections_;
  std::atomic<bool> connectionEventLost_{false};
  std::atomic<bool> advertising_{false};

  std::atomic<bool> enabled_{false};
  std::atomic<uint16_t> inputHandle_{0};
  std::atomic<uint16_t> outputHandle_{0};
  std::atomic<uint16_t> batteryHandle_{0};
  std::atomic<uint16_t> quotaHandle_{0};
  uint16_t connectionCount_ = 0;
};
