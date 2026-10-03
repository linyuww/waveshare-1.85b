#pragma once
#include <cstddef>
#include <cstdint>
#include "esp_err.h"

namespace shared_audio {
enum class Source { Chime, Assistant, Music };
constexpr int sample_rate = 24000;
esp_err_t init();
esp_err_t enableMicrophone();
esp_err_t read(int16_t *samples, size_t count);
esp_err_t write(Source source, const int16_t *samples, size_t count, int source_rate = sample_rate);
void prioritize(Source source, bool active);
esp_err_t setVolume(int percent);
int volume();
}
