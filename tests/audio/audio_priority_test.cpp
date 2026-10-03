#include "shared_audio.hpp"
#include "esp_codec_dev.h"
#include "freertos/task.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
std::vector<int16_t> output;
bool preempt_next = false;
int hardware_volume = 0;
int opened = 0;
}
esp_codec_dev_handle_t bsp_audio_codec_speaker_init() { return reinterpret_cast<void *>(1); }
esp_codec_dev_handle_t bsp_audio_codec_microphone_init() { return reinterpret_cast<void *>(2); }
int esp_codec_dev_open(esp_codec_dev_handle_t, esp_codec_dev_sample_info_t *format)
{
    assert(format->sample_rate == 24000 && format->channel == 1 && format->bits_per_sample == 16);
    ++opened;
    return ESP_CODEC_DEV_OK;
}
int esp_codec_dev_write(esp_codec_dev_handle_t, void *samples, int bytes)
{
    const auto *pcm = static_cast<const int16_t *>(samples);
    output.insert(output.end(), pcm, pcm + bytes / 2);
    if (preempt_next) {
        preempt_next = false;
        shared_audio::prioritize(shared_audio::Source::Chime, true);
    }
    return ESP_CODEC_DEV_OK;
}
int esp_codec_dev_read(esp_codec_dev_handle_t, void *samples, int bytes) { memset(samples, 0, bytes); return ESP_CODEC_DEV_OK; }
int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t, int volume) { hardware_volume = volume; return ESP_CODEC_DEV_OK; }
int esp_codec_dev_set_in_gain(esp_codec_dev_handle_t, float) { return ESP_CODEC_DEV_OK; }
void esp_codec_dev_delete(esp_codec_dev_handle_t) {}

int main()
{
    using namespace shared_audio;
    assert(init() == ESP_OK && init() == ESP_OK && opened == 1);
    assert(enableMicrophone() == ESP_OK && enableMicrophone() == ESP_OK && opened == 2);
    int16_t pcm[480] = {};
    assert(read(pcm, 480) == ESP_OK);
    assert(write(Source::Music, pcm, 480) == ESP_OK && output.size() == 480);
    output.clear();
    prioritize(Source::Assistant, true);
    assert(write(Source::Music, pcm, 480) == ESP_OK && output.empty());
    assert(preview_audio_delay == 20);
    assert(write(Source::Assistant, pcm, 480) == ESP_OK && output.size() == 480);
    output.clear();
    prioritize(Source::Chime, true);
    assert(write(Source::Assistant, pcm, 480) == ESP_OK && output.empty());
    assert(write(Source::Music, pcm, 480) == ESP_OK && output.empty());
    assert(write(Source::Chime, pcm, 480) == ESP_OK && output.size() == 480);
    assert(read(pcm, 480) == ESP_OK);
    prioritize(Source::Chime, false);
    prioritize(Source::Assistant, false);
    output.clear();
    preempt_next = true;
    preview_audio_delay = 0;
    assert(write(Source::Music, pcm, 480) == ESP_OK && output.size() == 240);
    assert(preview_audio_delay == 10);
    prioritize(Source::Chime, false);
    output.clear();
    assert(write(Source::Music, pcm, 480, 16000) == ESP_OK && output.size() == 720);
    assert(setVolume(0) == ESP_OK && volume() == 0 && hardware_volume == 0);
    assert(setVolume(100) == ESP_OK && volume() == 100 && hardware_volume == 100);
    assert(setVolume(101) == ESP_ERR_INVALID_ARG && volume() == 100);
    assert(write(Source::Music, pcm, 480, 441) == ESP_ERR_INVALID_ARG);
    puts("PASS: Codex chime > assistant speech > music, mid-buffer preemption, shared input, resampling and volume");
}
