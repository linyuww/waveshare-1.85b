#include "shared_audio.hpp"
#include <algorithm>
#include <atomic>
#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace shared_audio {
namespace {
SemaphoreHandle_t output_mutex;
SemaphoreHandle_t input_mutex;
esp_codec_dev_handle_t speaker;
esp_codec_dev_handle_t microphone;
bool microphone_open = false;
std::atomic<bool> chime_active{false};
std::atomic<bool> speech_active{false};
std::atomic<int> volume_percent{60};
esp_codec_dev_sample_info_t format()
{
    esp_codec_dev_sample_info_t info = {};
    info.sample_rate = sample_rate;
    info.bits_per_sample = 16;
    info.channel = 1;
    info.channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0);
    return info;
}
bool preempted(Source source)
{
    return source != Source::Chime && (chime_active.load() || (source == Source::Music && speech_active.load()));
}
void skipDuration(size_t count)
{
    const auto ticks = pdMS_TO_TICKS(count * 1000 / sample_rate);
    if (ticks) vTaskDelay(ticks);
}
}

esp_err_t init()
{
    if (!output_mutex) output_mutex = xSemaphoreCreateMutex();
    if (!input_mutex) input_mutex = xSemaphoreCreateMutex();
    if (!output_mutex || !input_mutex) return ESP_ERR_NO_MEM;
    xSemaphoreTake(output_mutex, portMAX_DELAY);
    esp_err_t result = ESP_OK;
    if (!speaker) {
        speaker = bsp_audio_codec_speaker_init();
        auto info = format();
        if (!speaker || esp_codec_dev_open(speaker, &info) != ESP_CODEC_DEV_OK) {
            if (speaker) esp_codec_dev_delete(speaker);
            speaker = nullptr;
            result = ESP_FAIL;
        } else {
            esp_codec_dev_set_out_vol(speaker, volume_percent.load());
        }
    }
    xSemaphoreGive(output_mutex);
    return result;
}

esp_err_t enableMicrophone()
{
    if (!input_mutex || !speaker) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(input_mutex, portMAX_DELAY);
    xSemaphoreTake(output_mutex, portMAX_DELAY);
    esp_err_t result = ESP_OK;
    if (!microphone_open) {
        if (!microphone) microphone = bsp_audio_codec_microphone_init();
        auto info = format();
        if (!microphone || esp_codec_dev_open(microphone, &info) != ESP_CODEC_DEV_OK) result = ESP_FAIL;
        else {
            microphone_open = true;
            esp_codec_dev_set_in_gain(microphone, 24.0f);
        }
    }
    xSemaphoreGive(output_mutex);
    xSemaphoreGive(input_mutex);
    return result;
}

esp_err_t read(int16_t *samples, size_t count)
{
    if (!input_mutex || !microphone_open || !samples || !count) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(input_mutex, portMAX_DELAY);
    const esp_err_t result = esp_codec_dev_read(microphone, samples, count * sizeof(int16_t)) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
    xSemaphoreGive(input_mutex);
    return result;
}

esp_err_t write(Source source, const int16_t *samples, size_t count, int source_rate)
{
    if (!output_mutex || !speaker || !samples || !count || source_rate < 8000 || source_rate > 48000) return ESP_ERR_INVALID_ARG;
    int16_t converted[240];
    const size_t output_count = count * sample_rate / source_rate;
    for (size_t offset = 0; offset < output_count; offset += 240) {
        if (preempted(source)) {
            skipDuration(output_count - offset);
            return ESP_OK;
        }
        xSemaphoreTake(output_mutex, portMAX_DELAY);
        if (preempted(source)) {
            xSemaphoreGive(output_mutex);
            skipDuration(output_count - offset);
            return ESP_OK;
        }
        const size_t chunk = std::min<size_t>(240, output_count - offset);
        for (size_t index = 0; index < chunk; ++index) {
            const size_t position = (offset + index) * source_rate / sample_rate;
            converted[index] = samples[std::min(position, count - 1)];
        }
        const int result = esp_codec_dev_write(speaker, converted, chunk * sizeof(int16_t));
        xSemaphoreGive(output_mutex);
        if (result != ESP_CODEC_DEV_OK) return ESP_FAIL;
        taskYIELD();
    }
    return ESP_OK;
}

void prioritize(Source source, bool active)
{
    if (source == Source::Chime) chime_active = active;
    if (source == Source::Assistant) speech_active = active;
}

esp_err_t setVolume(int percent)
{
    if (percent < 0 || percent > 100) return ESP_ERR_INVALID_ARG;
    if (!output_mutex || !speaker) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(output_mutex, portMAX_DELAY);
    const esp_err_t result = esp_codec_dev_set_out_vol(speaker, percent) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
    if (result == ESP_OK) volume_percent = percent;
    xSemaphoreGive(output_mutex);
    return result;
}

int volume() { return volume_percent.load(); }
}
