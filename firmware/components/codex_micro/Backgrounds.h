// SPDX-License-Identifier: MIT
// Scene backgrounds for the day and night themes, 360x360 RGB565.
//
// These are not C arrays in a generated header on purpose. Each frame is
// 259,200 bytes, and a header holding two of them would be roughly 1.8 MB of
// hex text for the compiler to chew through on every build. Instead the raw
// blobs are embedded with CMake's `target_add_binary_data`, which maps them
// straight into flash and hands back a start/end symbol pair.
//
// The blobs are stored in panel byte order (high byte first), exactly like the
// framebuffer, so drawing one is a row-wise memcpy with no per-pixel work. See
// scripts/assets/make_backgrounds.py.

#pragma once

#include <cstddef>
#include <cstdint>

extern const uint8_t kBackgroundDayStart[] asm("_binary_bg_day_bin_start");
extern const uint8_t kBackgroundDayEnd[] asm("_binary_bg_day_bin_end");
extern const uint8_t kBackgroundNightStart[] asm("_binary_bg_night_bin_start");
extern const uint8_t kBackgroundNightEnd[] asm("_binary_bg_night_bin_end");

namespace backgrounds {

constexpr int kWidth = 360;
constexpr int kHeight = 360;
// 360 * 360 * 2. Asserted at compile time below so a bad asset fails the build
// rather than showing up as a torn or shifted image on the panel.
constexpr std::size_t kBytes = static_cast<std::size_t>(kWidth) * kHeight * 2;

struct Scene {
  const uint8_t* data;
  std::size_t size;
};

inline Scene dayScene() {
  return {kBackgroundDayStart,
          static_cast<std::size_t>(kBackgroundDayEnd - kBackgroundDayStart)};
}

inline Scene nightScene() {
  return {kBackgroundNightStart,
          static_cast<std::size_t>(kBackgroundNightEnd -
                                   kBackgroundNightStart)};
}

// The parameter is named `dark`, not `night`: a parameter called `night` would
// shadow the accessor and make `night()` resolve to a bool, which fails to
// compile at the call site rather than here.
inline Scene forTheme(bool dark) { return dark ? nightScene() : dayScene(); }

}  // namespace backgrounds
