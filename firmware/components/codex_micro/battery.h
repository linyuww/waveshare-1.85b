// SPDX-License-Identifier: MIT
// BQ27220 fuel gauge on the Waveshare ESP32-S3-Touch-LCD-1.85B (I2C 0x55).
//
// The C152 port read M5.Power for battery telemetry. This board exposes a
// Texas Instruments BQ27220 gauge instead, which reports the same facts from a
// 64-byte register block.

#pragma once

#include <cstdint>

#include "esp_err.h"

namespace battery {

struct Sample {
  // False until the first successful read; percent stays -1 until then.
  bool valid = false;
  // Best estimate of the pack's state of charge, and the value the dashboard
  // and the BLE battery service publish. It is the gauge's own StateOfCharge
  // whenever that is fresh, and a voltage-advanced estimate in between -- see
  // the note on `socFromMillivolts` in battery_logic.h for why the gauge's
  // value cannot be trusted on its own.
  int percent = -1;
  // The raw StateOfCharge() the gauge reported, kept for logging. It trails
  // `percent` by however long the gauge has been asleep.
  int gaugePercent = -1;
  bool charging = false;
  bool discharging = false;
  // True when a USB host is attached or the gauge reports that the pack is not
  // discharging. Drives the powered artwork and the dock idle policy.
  bool externalPower = false;
  uint16_t voltageMv = 0;
  int16_t currentMa = 0;
  uint16_t fullChargeCapacityMah = 0;
  uint16_t remainingCapacityMah = 0;
  int healthPercent = -1;
  int temperatureDeciC = 0;
  uint16_t statusBits = 0;
  uint16_t rawCoulombCount = 0;
  int16_t averageCurrentMa = 0;
  int16_t averagePowerMw = 0;
  int internalTemperatureDeciC = 0;
  uint16_t cycleCount = 0;
  uint16_t designCapacityMah = 0;
  uint16_t timeToEmptyMin = 65535;
  uint16_t timeToFullMin = 65535;
  uint16_t desiredVoltageMv = 65535;
  uint16_t desiredCurrentMa = 65535;
  uint16_t operationBits = 0;
  uint32_t sampledAtMs = 0;
  bool stale = true;
};

// Probes the gauge once. Returns ESP_OK when the part answers with a plausible
// battery voltage.
esp_err_t init();

// Reads the gauge and updates the cached sample. Safe to call often; the
// register block is only re-read when the cache is older than the refresh
// interval.
Sample read(uint32_t nowMs);

// The most recent successful sample.
const Sample& cached();

}  // namespace battery
