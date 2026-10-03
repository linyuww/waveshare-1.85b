// SPDX-License-Identifier: MIT
// Copyright (c) 2026 imliubo
// StopWatch port changes copyright (c) 2026 Codex Micro for StopWatch contributors
// Waveshare ESP32-S3-Touch-LCD-1.85B port copyright (c) 2026 Codex Micro port
//
// Pure decision logic shared with the C152 firmware. These units are free of
// hardware and JSON-library dependencies so they can be reasoned about (and
// tested) in isolation.

#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "cJSON.h"

// ------------------------------------------------------- connection health ---
// Bluedroid can deliver disconnect and reconnect callbacks before the main
// loop gets another turn. Preserve those ordered conn_id transitions instead of
// sampling only the final aggregate count, which could otherwise make a new
// host session inherit the previous session's CODEX LIVE state.
namespace connection_health {

enum class Link : std::uint8_t {
  Offline,
  BleOnly,
  CodexLive,
};

enum class Quota : std::uint8_t {
  Waiting,
  Fresh,
  Stale,
};

struct Input {
  bool bleConnected = false;
  bool hostRpcObserved = false;
  std::uint32_t lastHostRpcAtMs = 0;
  std::uint32_t quotaWaitingSinceMs = 0;
  bool quotaAvailable = false;
  std::uint32_t quotaReceivedAtMs = 0;
};

struct Result {
  Link link = Link::Offline;
  Quota quota = Quota::Waiting;
};

class ConnectionSet {
 public:
  static constexpr std::size_t kCapacity = 8;

  struct Transition {
    bool changed = false;
    bool becameConnected = false;
    bool becameDisconnected = false;
    bool overflow = false;
  };

  Transition apply(bool connected, std::uint16_t id) {
    const std::size_t before = count();
    if (connected) {
      for (const Slot& slot : slots_) {
        if (slot.active && slot.id == id) return {};
      }
      for (Slot& slot : slots_) {
        if (!slot.active) {
          slot.id = id;
          slot.active = true;
          return transition(before, before + 1);
        }
      }
      Transition result;
      result.overflow = true;
      return result;
    }

    for (Slot& slot : slots_) {
      if (slot.active && slot.id == id) {
        slot.active = false;
        return transition(before, before - 1);
      }
    }
    return {};
  }

  bool contains(std::uint16_t id) const {
    for (const Slot& slot : slots_) {
      if (slot.active && slot.id == id) return true;
    }
    return false;
  }

  std::size_t count() const {
    std::size_t result = 0;
    for (const Slot& slot : slots_) result += slot.active ? 1U : 0U;
    return result;
  }

  bool sameMembers(const ConnectionSet& other) const {
    if (count() != other.count()) return false;
    for (const Slot& slot : slots_) {
      if (slot.active && !other.contains(slot.id)) return false;
    }
    return true;
  }

 private:
  struct Slot {
    std::uint16_t id = 0;
    bool active = false;
  };

  static Transition transition(std::size_t before, std::size_t after) {
    Transition result;
    result.changed = before != after;
    result.becameConnected = before == 0 && after > 0;
    result.becameDisconnected = before > 0 && after == 0;
    return result;
  }

  std::array<Slot, kCapacity> slots_ = {};
};

// Unsigned subtraction deliberately handles millis() rollover.
inline std::uint32_t age(std::uint32_t nowMs, std::uint32_t thenMs) {
  return nowMs - thenMs;
}

inline Result evaluate(const Input& input, std::uint32_t nowMs,
                       std::uint32_t hostRpcTtlMs, std::uint32_t quotaTtlMs) {
  Result result;

  result.link = !input.bleConnected
                    ? Link::Offline
                    : (input.hostRpcObserved &&
                               age(nowMs, input.lastHostRpcAtMs) <= hostRpcTtlMs
                           ? Link::CodexLive
                           : Link::BleOnly);

  if (input.quotaAvailable) {
    result.quota = age(nowMs, input.quotaReceivedAtMs) <= quotaTtlMs
                       ? Quota::Fresh
                       : Quota::Stale;
  } else if (input.bleConnected) {
    result.quota = age(nowMs, input.quotaWaitingSinceMs) <= quotaTtlMs
                       ? Quota::Waiting
                       : Quota::Stale;
  } else {
    result.quota = Quota::Waiting;
  }
  return result;
}

}  // namespace connection_health

// ------------------------------------------------------------ host RPC ------
namespace host_rpc {

enum class Method : std::uint8_t {
  Unsupported,
  SystemVersion,
  DeviceStatus,
  ThreadStatus,
  RgbConfig,
  LightsPreview,
  HostFocusedApp,
};

inline Method classify(const cJSON* request) {
  if (request == nullptr) return Method::Unsupported;
  const cJSON* method = cJSON_GetObjectItemCaseSensitive(request, "method");
  const cJSON* params = cJSON_GetObjectItemCaseSensitive(request, "params");
  if (!cJSON_IsString(method) || method->valuestring == nullptr) {
    return Method::Unsupported;
  }
  const char* name = method->valuestring;

  if (std::strcmp(name, "sys.version") == 0) return Method::SystemVersion;
  if (std::strcmp(name, "device.status") == 0) return Method::DeviceStatus;
  if (std::strcmp(name, "v.oai.thstatus") == 0 && cJSON_IsArray(params)) {
    return Method::ThreadStatus;
  }
  if (std::strcmp(name, "v.oai.rgbcfg") == 0 &&
      cJSON_IsObject(params)) {
    return Method::RgbConfig;
  }
  if (std::strcmp(name, "lights.preview") == 0) return Method::LightsPreview;
  if (std::strcmp(name, "host.focused_app") == 0) return Method::HostFocusedApp;
  return Method::Unsupported;
}

}  // namespace host_rpc

// ---------------------------------------------------------- quota payload ---
namespace quota_payload {

struct Snapshot {
  float fiveHourRemainingPercent = 0.0f;
  std::uint32_t fiveHourResetInSeconds = 0;
  float weeklyRemainingPercent = 0.0f;
  std::uint32_t weeklyResetInSeconds = 0;
};

inline bool parse(const cJSON* value, Snapshot& output) {
  if (!cJSON_IsObject(value)) return false;
  const cJSON* fiveHourRemaining = cJSON_GetObjectItemCaseSensitive(
      value, "five_hour_remaining_percent");
  const cJSON* fiveHourReset = cJSON_GetObjectItemCaseSensitive(
      value, "five_hour_reset_in_seconds");
  const cJSON* weeklyRemaining = cJSON_GetObjectItemCaseSensitive(
      value, "weekly_remaining_percent");
  const cJSON* weeklyReset = cJSON_GetObjectItemCaseSensitive(
      value, "weekly_reset_in_seconds");
  if (!cJSON_IsNumber(fiveHourRemaining) || !cJSON_IsNumber(fiveHourReset) ||
      !cJSON_IsNumber(weeklyRemaining) || !cJSON_IsNumber(weeklyReset)) {
    return false;
  }

  const double fiveHourPercent = fiveHourRemaining->valuedouble;
  const double weeklyPercent = weeklyRemaining->valuedouble;
  const double fiveHourSeconds = fiveHourReset->valuedouble;
  const double weeklySeconds = weeklyReset->valuedouble;
  if (!std::isfinite(fiveHourPercent) || fiveHourPercent < 0.0 ||
      fiveHourPercent > 100.0 || !std::isfinite(weeklyPercent) ||
      weeklyPercent < 0.0 || weeklyPercent > 100.0 ||
      !std::isfinite(fiveHourSeconds) || fiveHourSeconds < 0.0 ||
      !std::isfinite(weeklySeconds) || weeklySeconds < 0.0) {
    return false;
  }

  output.fiveHourRemainingPercent = static_cast<float>(fiveHourPercent);
  output.fiveHourResetInSeconds =
      static_cast<std::uint32_t>(fiveHourSeconds);
  output.weeklyRemainingPercent = static_cast<float>(weeklyPercent);
  output.weeklyResetInSeconds = static_cast<std::uint32_t>(weeklySeconds);
  return true;
}

}  // namespace quota_payload

// --------------------------------------------------------- touch gestures ---
namespace touch_gesture {

enum class Direction : std::int8_t {
  None = -1,
  Up = 0,
  Right = 1,
  Down = 2,
  Left = 3,
};

inline Direction classifySwipe(int dx, int dy, int threshold) {
  const std::int64_t distanceSquared =
      static_cast<std::int64_t>(dx) * dx + static_cast<std::int64_t>(dy) * dy;
  const std::int64_t thresholdSquared =
      static_cast<std::int64_t>(threshold) * threshold;
  if (distanceSquared < thresholdSquared) return Direction::None;

  const int absoluteX = dx < 0 ? -dx : dx;
  const int absoluteY = dy < 0 ? -dy : dy;
  if (absoluteX >= absoluteY) {
    return dx >= 0 ? Direction::Right : Direction::Left;
  }
  return dy >= 0 ? Direction::Down : Direction::Up;
}

inline float normalizedAngle(Direction direction) {
  switch (direction) {
    case Direction::Right: return 0.00f;
    case Direction::Down:  return 0.25f;
    case Direction::Left:  return 0.50f;
    case Direction::Up:    return 0.75f;
    case Direction::None:  return 0.00f;
  }
  return 0.00f;
}

inline const char* name(Direction direction) {
  switch (direction) {
    case Direction::Up:    return "UP";
    case Direction::Right: return "RIGHT";
    case Direction::Down:  return "DOWN";
    case Direction::Left:  return "LEFT";
    case Direction::None:  return "NONE";
  }
  return "NONE";
}

}  // namespace touch_gesture

// ------------------------------------------------------------ day / night ---
// The clock comes from NTP over Wi-Fi, so the theme boundary is evaluated in
// LOCAL time, not UTC -- using UTC would shift the whole cycle by eight hours.
//
// Two thresholds are needed, not one. A single "switch to night at 19:00"
// leaves the other half of the cycle undefined: the device would go dark at
// seven and never come back. The morning threshold is what closes the loop.
namespace theme {

constexpr int kNightStartHour = 19;  // >= 19:00 -> night
constexpr int kDayStartHour = 6;     // >= 06:00 -> day

// Callers must not evaluate this before the first NTP sync. Until then the
// hour is meaningless, and treating an unknown time as 0 would force night.
inline bool isNight(int hour) {
  return hour >= kNightStartHour || hour < kDayStartHour;
}

inline const char* name(bool night) { return night ? "night" : "day"; }

}  // namespace theme

// ------------------------------------------------------- completion banner --
namespace completion_banner {

struct Timer {
  std::int8_t agent = -1;
  std::uint32_t until_ms = 0;

  void show(std::int8_t completed_agent, std::uint32_t now,
            std::uint32_t duration_ms) {
    agent = completed_agent;
    until_ms = now + duration_ms;
  }

  bool visible(std::uint32_t now) const {
    return agent >= 0 && static_cast<std::int32_t>(until_ms - now) > 0;
  }

  bool expire(std::uint32_t now) {
    if (agent < 0 || visible(now)) return false;
    agent = -1;
    until_ms = 0;
    return true;
  }
};

}  // namespace completion_banner
