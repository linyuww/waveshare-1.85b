// SPDX-License-Identifier: MIT

#include "chime.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "board_config.h"
#include "board_i2c.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace chime {
namespace {

constexpr char kTag[] = "chime";

// ES8311 default 7-bit address (CE pin low). The Waveshare BSP quotes the
// 8-bit form 0x30; IDF's I2C master takes the 7-bit form.
constexpr uint8_t kCodecAddress = 0x18;

// The BSP ships 22050 Hz as the proven duplex rate for this codec. MCLK is
// 256 x fs = 5.6448 MHz, which is one of the ES8311's documented coefficients.
constexpr uint32_t kSampleRate = 22050;
constexpr uint32_t kMclkMultiple = 256;

constexpr float kPi = 3.14159265358979323846f;
constexpr uint32_t kChimeDurationMs = 300;
constexpr size_t kChimeSamples = kSampleRate * kChimeDurationMs / 1000;
constexpr int kVolumePercent = 60;

// ES8311 register map (subset).
constexpr uint8_t kRegReset = 0x00;
constexpr uint8_t kRegClk01 = 0x01;
constexpr uint8_t kRegClk02 = 0x02;
constexpr uint8_t kRegClk03 = 0x03;
constexpr uint8_t kRegClk04 = 0x04;
constexpr uint8_t kRegClk05 = 0x05;
constexpr uint8_t kRegClk06 = 0x06;
constexpr uint8_t kRegClk07 = 0x07;
constexpr uint8_t kRegClk08 = 0x08;
constexpr uint8_t kRegSdpIn = 0x09;
constexpr uint8_t kRegSdpOut = 0x0A;
constexpr uint8_t kRegSys0D = 0x0D;
constexpr uint8_t kRegSys0E = 0x0E;
constexpr uint8_t kRegSys12 = 0x12;
constexpr uint8_t kRegSys13 = 0x13;
constexpr uint8_t kRegAdc1C = 0x1C;
constexpr uint8_t kRegDac32 = 0x32;
constexpr uint8_t kRegDac37 = 0x37;

i2s_chan_handle_t s_tx = nullptr;
int16_t* s_pcm = nullptr;
bool s_ready = false;
bool s_enabled = true;

bool writeRegister(uint8_t reg, uint8_t value) {
  return board_i2c::writeRegister(kCodecAddress, reg, value) == ESP_OK;
}

bool readRegister(uint8_t reg, uint8_t& value) {
  return board_i2c::readRegister(kCodecAddress, reg, &value, 1) == ESP_OK;
}

bool updateRegister(uint8_t reg, uint8_t mask, uint8_t value) {
  uint8_t current = 0;
  if (!readRegister(reg, current)) return false;
  return writeRegister(reg, static_cast<uint8_t>((current & ~mask) | value));
}

// ES8311 clock coefficient for MCLK 5.6448 MHz @ 22050 Hz:
// pre_div=1, pre_multi=1x, adc_div=1, dac_div=1, single speed,
// lrck_h=0x00, lrck_l=0xFF, bclk_div=4, adc_osr=dac_osr=0x10.
bool configureCodecClock() {
  if (!updateRegister(kRegClk02, 0xF8, 0x00)) return false;
  if (!writeRegister(kRegClk03, 0x10)) return false;
  if (!writeRegister(kRegClk04, 0x10)) return false;
  if (!writeRegister(kRegClk05, 0x00)) return false;
  // bclk_div = 4 -> register field holds 3.
  if (!updateRegister(kRegClk06, 0x1F, 0x03)) return false;
  if (!updateRegister(kRegClk07, 0x3F, 0x00)) return false;
  if (!writeRegister(kRegClk08, 0xFF)) return false;
  return true;
}

bool configureCodec() {
  // Reset, then power-on. The codec needs the I2S MCLK already running for the
  // reset to settle into a usable state.
  if (!writeRegister(kRegReset, 0x1F)) return false;
  vTaskDelay(pdMS_TO_TICKS(20));
  if (!writeRegister(kRegReset, 0x00)) return false;
  if (!writeRegister(kRegReset, 0x80)) return false;

  // All clocks on, MCLK taken from the MCLK pin, neither clock inverted.
  if (!writeRegister(kRegClk01, 0x3F)) return false;
  if (!updateRegister(kRegClk06, 0x20, 0x00)) return false;

  if (!configureCodecClock()) return false;

  // Slave serial port, 16-bit in and out (3 << 2).
  if (!updateRegister(kRegReset, 0x40, 0x00)) return false;
  if (!writeRegister(kRegSdpIn, 0x0C)) return false;
  if (!writeRegister(kRegSdpOut, 0x0C)) return false;

  if (!writeRegister(kRegSys0D, 0x01)) return false;  // power up analog
  if (!writeRegister(kRegSys0E, 0x02)) return false;  // analog PGA + ADC
  if (!writeRegister(kRegSys12, 0x00)) return false;  // power up DAC
  if (!writeRegister(kRegSys13, 0x10)) return false;  // output to HP drive
  if (!writeRegister(kRegAdc1C, 0x6A)) return false;  // ADC EQ bypass
  if (!writeRegister(kRegDac37, 0x08)) return false;  // DAC EQ bypass

  const int volume = kVolumePercent <= 0 ? 0
                                         : (kVolumePercent * 256 / 100) - 1;
  if (!writeRegister(kRegDac32, static_cast<uint8_t>(volume))) return false;
  return true;
}

void setAmplifierEnabled(bool enabled) {
  gpio_set_level(BOARD_POWER_AMP, enabled ? 1 : 0);
}

float softNoteEnvelope(float noteTime, float noteDuration) {
  constexpr float kAttackSeconds = 0.038f;
  constexpr float kReleaseSeconds = 0.100f;
  if (noteTime < 0.0f || noteTime >= noteDuration) return 0.0f;

  float envelope = 1.0f;
  if (noteTime < kAttackSeconds) {
    const float phase = noteTime / kAttackSeconds;
    const float fade = sinf(phase * kPi * 0.5f);
    envelope *= fade * fade;
  }
  const float releaseStart = noteDuration - kReleaseSeconds;
  if (noteTime > releaseStart) {
    const float phase = (noteTime - releaseStart) / kReleaseSeconds;
    const float fade = cosf(phase * kPi * 0.5f);
    envelope *= fade * fade;
  }
  return envelope;
}

void buildCompletionChime() {
  // A subdued A4 -> C#5 major third with rounded attacks and long releases.
  // One PCM buffer avoids the hard edge between two separate tones.
  constexpr float kFirstFrequency = 440.00f;
  constexpr float kSecondFrequency = 554.37f;
  constexpr float kFirstStart = 0.0f;
  constexpr float kFirstDuration = 0.145f;
  constexpr float kSecondStart = 0.105f;
  constexpr float kSecondDuration = 0.165f;
  constexpr float kPeakAmplitude = 0.135f * 32767.0f;

  for (size_t sample = 0; sample < kChimeSamples; ++sample) {
    const float time = static_cast<float>(sample) / kSampleRate;
    const float firstTime = time - kFirstStart;
    const float secondTime = time - kSecondStart;
    const float first = sinf(2.0f * kPi * kFirstFrequency * firstTime) *
                        softNoteEnvelope(firstTime, kFirstDuration);
    const float second = sinf(2.0f * kPi * kSecondFrequency * secondTime) *
                         softNoteEnvelope(secondTime, kSecondDuration);
    s_pcm[sample] = static_cast<int16_t>(kPeakAmplitude * (first + second));
  }
}

}  // namespace

esp_err_t init() {
  if (s_ready) return ESP_OK;

  // The amplifier enable line is a plain GPIO on this board.
  const gpio_config_t ampConfig = {
      .pin_bit_mask = 1ULL << BOARD_POWER_AMP,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&ampConfig);
  setAmplifierEnabled(false);

  const i2s_chan_config_t channelConfig =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  esp_err_t result = i2s_new_channel(&channelConfig, &s_tx, nullptr);
  if (result != ESP_OK) {
    ESP_LOGW(kTag, "i2s_new_channel failed: %s", esp_err_to_name(result));
    return result;
  }

  i2s_std_config_t stdConfig = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate),
      .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                     I2S_SLOT_MODE_MONO),
      .gpio_cfg =
          {
              .mclk = BOARD_I2S_MCLK,
              .bclk = BOARD_I2S_BCLK,
              .ws = BOARD_I2S_LRCK,
              .dout = BOARD_I2S_DOUT,
              .din = I2S_GPIO_UNUSED,
              .invert_flags =
                  {
                      .mclk_inv = false,
                      .bclk_inv = false,
                      .ws_inv = false,
                  },
          },
  };
  stdConfig.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

  result = i2s_channel_init_std_mode(s_tx, &stdConfig);
  if (result != ESP_OK) {
    ESP_LOGW(kTag, "i2s_channel_init_std_mode failed: %s",
             esp_err_to_name(result));
    i2s_del_channel(s_tx);
    s_tx = nullptr;
    return result;
  }
  result = i2s_channel_enable(s_tx);
  if (result != ESP_OK) {
    ESP_LOGW(kTag, "i2s_channel_enable failed: %s", esp_err_to_name(result));
    i2s_del_channel(s_tx);
    s_tx = nullptr;
    return result;
  }

  // MCLK must be running before the codec is programmed.
  if (!configureCodec()) {
    ESP_LOGW(kTag, "ES8311 init failed; chime disabled");
    i2s_channel_disable(s_tx);
    i2s_del_channel(s_tx);
    s_tx = nullptr;
    return ESP_ERR_INVALID_RESPONSE;
  }

  s_pcm = static_cast<int16_t*>(malloc(sizeof(int16_t) * kChimeSamples));
  if (s_pcm == nullptr) {
    ESP_LOGW(kTag, "chime buffer allocation failed");
    i2s_channel_disable(s_tx);
    i2s_del_channel(s_tx);
    s_tx = nullptr;
    return ESP_ERR_NO_MEM;
  }
  buildCompletionChime();

  s_ready = true;
  ESP_LOGI(kTag, "ES8311 + I2S ready (%lu Hz, %u samples)",
           static_cast<unsigned long>(kSampleRate),
           static_cast<unsigned>(kChimeSamples));
  return ESP_OK;
}

bool ready() { return s_ready; }

void playCompletion() {
  if (!s_ready || !s_enabled || s_tx == nullptr || s_pcm == nullptr) return;
  setAmplifierEnabled(true);
  size_t written = 0;
  const esp_err_t result = i2s_channel_write(
      s_tx, s_pcm, sizeof(int16_t) * kChimeSamples, &written,
      pdMS_TO_TICKS(kChimeDurationMs + 200));
  if (result != ESP_OK) {
    ESP_LOGW(kTag, "i2s_channel_write failed: %s", esp_err_to_name(result));
  }
  // Let the DAC drain before cutting the amplifier, otherwise the tail clicks.
  vTaskDelay(pdMS_TO_TICKS(30));
  setAmplifierEnabled(false);
}

void setEnabled(bool enabled) { s_enabled = enabled; }

bool enabled() { return s_enabled; }

}  // namespace chime
