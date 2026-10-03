// SPDX-License-Identifier: MIT

#include "battery.h"

#include <cstring>

#include "battery_logic.h"
#include "board_config.h"
#include "board_i2c.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace battery {
namespace {

constexpr char kTag[] = "battery";

// Standard command registers. The gauge serves 0x00..0x3F as one readable
// block, so a single transaction refreshes every field we care about.
//
// Addresses follow the BQ27220 standard command map (TRM SLUUBD4). Note the
// map is NOT a dense 2-byte array: 0x26 is reserved, 0x2A is CycleCount, and
// 0x2C/0x2E are StateOfCharge/StateOfHealth rather than part of a
// RemainingCapacity ladder. Cross-checked against the mverch67/BQ27220
// driver, which reads the same fields one word at a time.
constexpr uint8_t kRegTemperature = 0x06;
constexpr uint8_t kRegVoltage = 0x08;
constexpr uint8_t kRegBatteryStatus = 0x0A;
constexpr uint8_t kRegCurrent = 0x0C;
constexpr uint8_t kRegRemainingCapacity = 0x10;
constexpr uint8_t kRegFullChargeCapacity = 0x12;
constexpr uint8_t kRegRawCoulombCount = 0x22;
constexpr uint8_t kRegStateOfCharge = 0x2C;
constexpr uint8_t kRegStateOfHealth = 0x2E;
constexpr uint8_t kBlockStart = 0x00;
constexpr size_t kBlockLength = 0x40;

// BatteryStatus bit 0 (DSG) is set while the pack is discharging.
constexpr uint16_t kStatusDischarging = 0x0001;

// Refresh cadence. The gauge updates its own SOC once per second, so anything
// faster only burns I2C bandwidth.
constexpr uint32_t kRefreshIntervalMs = 5000;

// A plausible single-cell Li-ion reading. Anything outside means the gauge is
// not populated (or not answering) on this board revision.
constexpr uint16_t kMinPlausibleMv = 2000;
constexpr uint16_t kMaxPlausibleMv = 4600;

// The gauge's Current() reads 0 mA on this board, so the current-based
// charging test can never pass. Fall back to "on external power and the
// estimate climbed over this window".
constexpr uint32_t kChargeEvidenceWindowMs = 10 * 60 * 1000;
constexpr int kChargeEvidenceRise = 1;

Sample s_sample;
uint32_t s_lastReadMs = 0;
bool s_probed = false;

// Turns the gauge's state of charge into something that keeps moving even
// while the gauge is asleep. See battery_logic.h for why.
battery_logic::SocTracker s_tracker;

// Charging evidence window.
uint32_t s_chargeWindowStartMs = 0;
int s_chargeWindowStartPercent = -1;
bool s_chargeInferred = false;

void refreshExternalPower() {
  s_sample.externalPower = battery_logic::hasExternalPower(
      usb_serial_jtag_is_connected(), s_sample.valid, s_sample.discharging);
}

inline uint16_t littleEndian16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline int16_t littleEndianS16(const uint8_t* p) {
  return static_cast<int16_t>(littleEndian16(p));
}

bool readBlock(uint8_t* block) {
  return board_i2c::readRegister(BOARD_I2C_ADDR_BQ27220, kBlockStart, block,
                                 kBlockLength) == ESP_OK;
}

// Compact one-line decode of a freshly read block.
void logGaugeBlock(const uint8_t* block, const char* why) {
  ESP_LOGI(kTag,
           "%s: volt=%u mV curr=%d mA rc=%u mAh fcc=%u mAh soc=%u%% "
           "soh=%u%% temp=%.1fK status=0x%04X rawcc=%u",
           why, littleEndian16(block + kRegVoltage),
           littleEndianS16(block + kRegCurrent),
           littleEndian16(block + kRegRemainingCapacity),
           littleEndian16(block + kRegFullChargeCapacity),
           littleEndian16(block + kRegStateOfCharge),
           littleEndian16(block + kRegStateOfHealth),
           littleEndian16(block + kRegTemperature) / 10.0f,
           littleEndian16(block + kRegBatteryStatus),
           littleEndian16(block + kRegRawCoulombCount));
  ESP_LOGI(kTag, "telemetry: avg_current=%d mA avg_power=%d mW internal_temp=%.1fK cycles=%u design=%u mAh tte=%u min ttf=%u min desired_voltage=%u mV desired_current=%u mA operation=0x%04X",
           littleEndianS16(block + 0x14), littleEndianS16(block + 0x24), littleEndian16(block + 0x28) / 10.0f,
           littleEndian16(block + 0x2A), littleEndian16(block + 0x3C), littleEndian16(block + 0x16), littleEndian16(block + 0x18),
           littleEndian16(block + 0x30), littleEndian16(block + 0x32), littleEndian16(block + 0x3A));
}

void updateEstimate(int gaugePercent, int voltageMv) {
  const int before = s_tracker.reported;
  const bool moved = s_tracker.update(gaugePercent, voltageMv);

  if (s_tracker.gaugeRefreshed && before >= 0 && before != s_tracker.reported) {
    ESP_LOGI(kTag, "gauge refreshed: soc=%d%% (estimate was %d%%) at %u mV",
             s_tracker.reported, before, voltageMv);
  } else if (moved) {
    ESP_LOGD(kTag, "gauge stale at %d%%; %d mV moves estimate %d->%d%%",
             gaugePercent, voltageMv, before, s_tracker.reported);
  }
}

// The gauge cannot tell us whether the pack is charging (Current() is 0), so
// watch the estimate instead: climbing while on external power is the only
// evidence available on this board.
void updateChargingEvidence(bool externalPower, uint32_t nowMs) {
  if (s_chargeWindowStartPercent < 0 ||
      nowMs - s_chargeWindowStartMs >= kChargeEvidenceWindowMs) {
    if (s_chargeWindowStartPercent >= 0 && s_tracker.reported >= 0) {
      s_chargeInferred =
          externalPower &&
          s_tracker.reported >= s_chargeWindowStartPercent + kChargeEvidenceRise;
    }
    s_chargeWindowStartMs = nowMs;
    s_chargeWindowStartPercent = s_tracker.reported;
  }
}

}  // namespace

esp_err_t init() {
  uint8_t block[kBlockLength] = {};
  if (!readBlock(block)) {
    ESP_LOGW(kTag, "BQ27220 did not answer at 0x%02X",
             BOARD_I2C_ADDR_BQ27220);
    return ESP_ERR_NOT_FOUND;
  }
  const uint16_t voltage = littleEndian16(block + kRegVoltage);
  if (voltage < kMinPlausibleMv || voltage > kMaxPlausibleMv) {
    ESP_LOGW(kTag, "BQ27220 reported implausible voltage %u mV", voltage);
    return ESP_ERR_INVALID_RESPONSE;
  }
  s_probed = true;
  ESP_LOGI(kTag, "BQ27220 online voltage=%u mV", voltage);
  logGaugeBlock(block, "boot");
  // Raw bytes, for the next time a field looks wrong. DEBUG keeps them out of
  // the normal boot log but one sdkconfig change brings them back.
  ESP_LOG_BUFFER_HEX_LEVEL(kTag, block, kBlockLength, ESP_LOG_DEBUG);
  return ESP_OK;
}

Sample read(uint32_t nowMs) {
  if (!s_probed) {
    s_probed = true;
    if (init() != ESP_OK) {
      // Leave percent at -1 so the dashboard renders "--%".
      return s_sample;
    }
    s_lastReadMs = 0;
  }

  if (s_lastReadMs != 0 && nowMs - s_lastReadMs < kRefreshIntervalMs) {
    // USB SOF presence can change between gauge refreshes, so never cache it.
    refreshExternalPower();
    return s_sample;
  }

  uint8_t block[kBlockLength] = {};
  if (!readBlock(block)) {
    ESP_LOGW(kTag, "BQ27220 read failed; keeping last sample");
    s_sample.stale = true;
    s_lastReadMs = nowMs;
    return s_sample;
  }

  const uint16_t voltage = littleEndian16(block + kRegVoltage);
  if (voltage < kMinPlausibleMv || voltage > kMaxPlausibleMv) {
    ESP_LOGW(kTag, "BQ27220 voltage out of range (%u mV); ignoring", voltage);
    s_sample.stale = true;
    s_lastReadMs = nowMs;
    return s_sample;
  }

  const uint16_t status = littleEndian16(block + kRegBatteryStatus);
  const int16_t current = littleEndianS16(block + kRegCurrent);
  const uint16_t soc = littleEndian16(block + kRegStateOfCharge);

  s_sample.voltageMv = voltage;
  s_sample.currentMa = current;
  s_sample.gaugePercent = soc <= 100 ? static_cast<int>(soc) : -1;
  s_sample.discharging = (status & kStatusDischarging) != 0;
  s_sample.fullChargeCapacityMah =
      littleEndian16(block + kRegFullChargeCapacity);
  s_sample.remainingCapacityMah = littleEndian16(block + kRegRemainingCapacity);
  const uint16_t health = littleEndian16(block + kRegStateOfHealth);
  s_sample.healthPercent = health <= 100 ? health : -1;
  s_sample.temperatureDeciC = static_cast<int>(littleEndian16(block + kRegTemperature)) - 2731;
  s_sample.statusBits = status;
  s_sample.rawCoulombCount = littleEndian16(block + kRegRawCoulombCount);
  s_sample.averageCurrentMa = littleEndianS16(block + 0x14);
  s_sample.averagePowerMw = littleEndianS16(block + 0x24);
  s_sample.internalTemperatureDeciC = static_cast<int>(littleEndian16(block + 0x28)) - 2731;
  s_sample.cycleCount = littleEndian16(block + 0x2A);
  s_sample.designCapacityMah = littleEndian16(block + 0x3C);
  s_sample.timeToEmptyMin = littleEndian16(block + 0x16);
  s_sample.timeToFullMin = littleEndian16(block + 0x18);
  s_sample.desiredVoltageMv = littleEndian16(block + 0x30);
  s_sample.desiredCurrentMa = littleEndian16(block + 0x32);
  s_sample.operationBits = littleEndian16(block + 0x3A);
  s_sample.sampledAtMs = nowMs;
  s_sample.stale = false;
  refreshExternalPower();

  updateEstimate(s_sample.gaugePercent, voltage);
  updateChargingEvidence(s_sample.externalPower, nowMs);
  s_sample.percent = s_tracker.reported;
  s_sample.charging = battery_logic::isCharging(current, s_sample.discharging) ||
                      s_chargeInferred;
  s_sample.valid = s_sample.percent >= 0;

  s_lastReadMs = nowMs;
  ESP_LOGD(kTag, "soc=%d%% (gauge %d%%) %u mV %d mA dsg=%d charging=%d ext=%d",
           s_sample.percent, s_sample.gaugePercent, voltage, current,
           s_sample.discharging ? 1 : 0, s_sample.charging ? 1 : 0,
           s_sample.externalPower ? 1 : 0);
  return s_sample;
}

const Sample& cached() { return s_sample; }

}  // namespace battery
