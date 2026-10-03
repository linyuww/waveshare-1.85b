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

// Brings up I2S TX and the ES8311 codec. Returns ESP_OK only when both the I2S
// channel and the codec answered; callers must treat failure as non-fatal.
esp_err_t init();

bool ready();

// Plays the completion chime. Blocking for roughly the length of the sample
// (about 300 ms). Silently does nothing when the codec is unavailable.
void playCompletion();

// 0 disables playback without tearing the codec down.
void setEnabled(bool enabled);
bool enabled();

}  // namespace chime
