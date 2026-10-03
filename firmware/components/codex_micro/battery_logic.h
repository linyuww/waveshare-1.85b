// SPDX-License-Identifier: MIT
// Pure battery decisions shared by the gauge driver and dashboard.

#pragma once

#include <cstdint>

namespace battery_logic {

constexpr int kChargeCurrentThresholdMa = 20;

constexpr int clampPercent(int percent) {
  return percent < 0 ? -1 : (percent > 100 ? 100 : percent);
}

// Five inclusive bands: 0..20, 21..40, 41..60, 61..80, 81..100.
constexpr int iconLevel(int percent) {
  const int clamped = clampPercent(percent);
  return clamped <= 0 ? 1 : (clamped + 19) / 20;
}

constexpr bool isCharging(int currentMa, bool discharging) {
  return !discharging && currentMa >= kChargeCurrentThresholdMa;
}

constexpr bool hasExternalPower(bool usbHostConnected, bool gaugeValid,
                                bool discharging) {
  return usbHostConnected || (gaugeValid && !discharging);
}

// --- Keeping the state of charge alive -------------------------------------
//
// The BQ27220 on this board stops gauging and never picks itself back up. It
// parks in SLEEP (BatteryStatus bit 12), Current() reads 0 mA and
// RawCoulombCount never moves, so StateOfCharge() only changes when the gauge
// re-derives it from an open-circuit measurement -- and that can be hours
// apart. Measured on 2026-09-25: the register block was byte-for-byte
// identical for minutes at a time while the pack went from 3.75 V to 4.13 V,
// and the reported percentage sat at 32 % the whole way. The gauge's Control()
// interface does not answer on this part (every subcommand reads back 0x00FF),
// so the firmware cannot wake it or ask for a fresh estimate either.
//
// Voltage() is the one field the gauge does keep current, so when the reported
// percentage stops tracking the pack the driver advances the last trustworthy
// value along this curve instead of showing a frozen number.
constexpr int kOcvMv[] = {3300, 3400, 3450, 3500, 3550, 3600, 3650, 3700,
                          3750, 3800, 3850, 3900, 3950, 4000, 4050, 4100,
                          4150, 4200};
constexpr int kOcvPercent[] = {0,  5,  8,  12, 17, 23, 30, 38,
                               46, 55, 63, 70, 76, 82, 87, 91,
                               96, 100};
constexpr int kOcvPoints = sizeof(kOcvMv) / sizeof(kOcvMv[0]);

// Single-cell Li-ion open-circuit voltage to percent, piecewise linear.
constexpr int socFromMillivolts(int mv) {
  if (mv <= kOcvMv[0]) return 0;
  if (mv >= kOcvMv[kOcvPoints - 1]) return 100;
  int i = kOcvPoints - 2;
  while (i > 0 && mv < kOcvMv[i]) --i;
  const int span = kOcvMv[i + 1] - kOcvMv[i];
  const int offset = mv - kOcvMv[i];
  return kOcvPercent[i] +
         (kOcvPercent[i + 1] - kOcvPercent[i]) * offset / span;
}

// How far the cell voltage has to move away from the voltage that anchored the
// last trustworthy gauge reading before we stop believing that reading. Small
// enough to catch a real charge or discharge, large enough to ignore the
// gauge's 1 mV resolution and the odd load step.
constexpr int kStaleDriftMv = 30;

// Keeps the reported state of charge moving while the gauge sits on a stale
// value.
//
// A gauge value we have not seen before is taken at face value and becomes the
// new anchor. While the gauge keeps repeating the same value the cell voltage
// is the only thing still moving, so the estimate is re-derived from the
// anchor every time the voltage drifts past kStaleDriftMv. That keeps the
// dashboard honest without inventing a second, disagreeing scale: every
// estimate starts from a number the gauge itself produced, and snaps back to
// the gauge the moment the gauge produces a new one.
struct SocTracker {
  // What the dashboard and the BLE battery service publish. -1 = unknown.
  int reported = -1;
  // The gauge reading the estimate is currently anchored to.
  int anchorPercent = -1;
  // Cell voltage when that anchor was taken.
  int anchorMv = 0;
  // Last value seen from the gauge, used to spot a genuinely new reading.
  int lastGaugePercent = -1;
  // Set by update() when the gauge produced a value we had not seen.
  bool gaugeRefreshed = false;

  // Returns true when `reported` changed.
  constexpr bool update(int gaugePercent, int voltageMv) {
    gaugeRefreshed = false;
    if (gaugePercent < 0) return false;  // gauge not answering; hold

    const bool fresh = anchorPercent < 0 || gaugePercent != lastGaugePercent;
    lastGaugePercent = gaugePercent;

    if (fresh) {
      anchorPercent = gaugePercent;
      anchorMv = voltageMv;
      gaugeRefreshed = true;
      const bool changed = reported != gaugePercent;
      reported = gaugePercent;
      return changed;
    }

    const int drift = voltageMv - anchorMv;
    if (drift < kStaleDriftMv && drift > -kStaleDriftMv) return false;

    const int advanced =
        anchorPercent + socFromMillivolts(voltageMv) -
        socFromMillivolts(anchorMv);
    const int clamped = advanced < 0 ? 0 : (advanced > 100 ? 100 : advanced);
    const bool changed = reported != clamped;
    reported = clamped;
    return changed;
  }
};

// Boundary and truth-table regression checks compile with the firmware.
static_assert(iconLevel(-1) == 1);
static_assert(iconLevel(0) == 1);
static_assert(iconLevel(1) == 1);
static_assert(iconLevel(20) == 1);
static_assert(iconLevel(21) == 2);
static_assert(iconLevel(40) == 2);
static_assert(iconLevel(41) == 3);
static_assert(iconLevel(60) == 3);
static_assert(iconLevel(61) == 4);
static_assert(iconLevel(80) == 4);
static_assert(iconLevel(81) == 5);
static_assert(iconLevel(100) == 5);
static_assert(iconLevel(101) == 5);

static_assert(hasExternalPower(true, false, true));
static_assert(hasExternalPower(true, true, true));
static_assert(hasExternalPower(false, true, false));
static_assert(!hasExternalPower(false, true, true));
static_assert(!hasExternalPower(false, false, false));
static_assert(isCharging(20, false));
static_assert(!isCharging(19, false));
static_assert(!isCharging(20, true));

static_assert(socFromMillivolts(3200) == 0);
static_assert(socFromMillivolts(3300) == 0);
static_assert(socFromMillivolts(3700) == 38);
static_assert(socFromMillivolts(3750) == 46);
static_assert(socFromMillivolts(3900) == 70);
static_assert(socFromMillivolts(4133) == 94);
static_assert(socFromMillivolts(4200) == 100);
static_assert(socFromMillivolts(4300) == 100);

// The tracker is what turns a frozen gauge into a moving percentage, so pin
// its behaviour down at compile time.
//
// 2026-09-25 regression: gauge stuck at 32 % from 3.75 V all the way up to
// 4.13 V.
constexpr bool trackerFollowsTheCell() {
  SocTracker t;
  t.update(32, 3750);
  t.update(32, 3750);  // gauge repeats itself: stale
  const bool moved = t.update(32, 3900);
  return moved && t.reported == 56;
}
constexpr bool trackerReanchorsWhenTheGaugeWakesUp() {
  SocTracker t;
  t.update(32, 3750);
  t.update(32, 3900);
  const bool moved = t.update(97, 4133);
  return moved && t.reported == 97 && t.gaugeRefreshed;
}
constexpr bool trackerIgnoresLoadSteps() {
  SocTracker t;
  t.update(50, 3900);
  const bool moved = t.update(50, 3920);  // 20 mV < kStaleDriftMv
  return !moved && t.reported == 50;
}
constexpr bool trackerHoldsWhenTheGaugeIsSilent() {
  SocTracker t;
  const bool moved = t.update(-1, 4000);
  return !moved && t.reported == -1;
}
constexpr bool trackerClampsToZero() {
  SocTracker t;
  t.update(3, 3800);
  t.update(3, 3300);  // a whole pack's worth of voltage drop
  return t.reported == 0;
}
static_assert(trackerFollowsTheCell());
static_assert(trackerReanchorsWhenTheGaugeWakesUp());
static_assert(trackerIgnoresLoadSteps());
static_assert(trackerHoldsWhenTheGaugeIsSilent());
static_assert(trackerClampsToZero());

}  // namespace battery_logic
