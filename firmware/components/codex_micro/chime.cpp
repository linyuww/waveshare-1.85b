#include "chime.h"
#include <atomic>
#include <cmath>
#include <cstdlib>
#include "shared_audio.hpp"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace chime {
namespace {
constexpr int sample_count = shared_audio::sample_rate * 300 / 1000;
int16_t *pcm;
SemaphoreHandle_t mutex;
std::atomic<bool> is_enabled{true};
}
esp_err_t init()
{
    if (pcm) return ESP_OK;
    const esp_err_t result = shared_audio::init();
    if (result != ESP_OK) return result;
    mutex = xSemaphoreCreateMutex();
    pcm = static_cast<int16_t *>(malloc(sample_count * sizeof(int16_t)));
    if (!mutex || !pcm) {
        free(pcm);
        pcm = nullptr;
        if (mutex) vSemaphoreDelete(mutex);
        mutex = nullptr;
        return ESP_ERR_NO_MEM;
    }
    for (int index = 0; index < sample_count; ++index) {
        const float time = static_cast<float>(index) / shared_audio::sample_rate;
        const float first = time < 0.14f ? sinf(2.0f * 3.14159265f * 440 * time) * sinf(3.14159265f * time / 0.14f) : 0;
        const float second_time = time - 0.12f;
        const float second = second_time >= 0 && second_time < 0.18f ? sinf(2.0f * 3.14159265f * 554.365f * second_time) * sinf(3.14159265f * second_time / 0.18f) : 0;
        pcm[index] = static_cast<int16_t>(6500 * (first + second));
    }
    return ESP_OK;
}
bool ready() { return pcm != nullptr; }
void playCompletion()
{
    if (!pcm || !is_enabled.load() || xSemaphoreTake(mutex, 0) != pdTRUE) return;
    shared_audio::prioritize(shared_audio::Source::Chime, true);
    const auto result = shared_audio::write(shared_audio::Source::Chime, pcm, sample_count);
    shared_audio::prioritize(shared_audio::Source::Chime, false);
    xSemaphoreGive(mutex);
    if (result != ESP_OK) ESP_LOGW("chime", "Playback failed: %s", esp_err_to_name(result));
}
void setEnabled(bool enabled) { is_enabled = enabled; }
bool enabled() { return is_enabled.load(); }
}
