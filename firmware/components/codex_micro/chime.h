// SPDX-License-Identifier: MIT
// Completion chime for the Waveshare ESP32-S3-Touch-LCD-1.85B.
//
// The C152 firmware drove an internal speaker through M5Unified. This board has
// an ES8311 DAC behind an I2S amplifier, so the same synthesized A4 -> C#5
// major third is rendered here and pushed out over I2S.

#pragma once

#include <cstdint>

#include "esp_err.h"

namespace chime {

// Uses the shared BSP audio driver. Failure is non-fatal.
esp_err_t init();

bool ready();

// Plays the completion chime. Blocking for roughly the length of the sample
// (about 300 ms). Silently does nothing when the codec is unavailable.
void playCompletion();

// Disables playback without tearing the shared codec down.
void setEnabled(bool enabled);
bool enabled();

}  // namespace chime
