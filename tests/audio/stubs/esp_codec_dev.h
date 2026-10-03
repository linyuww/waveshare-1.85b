#pragma once
#include <cstdint>
using esp_codec_dev_handle_t = void *;
constexpr int ESP_CODEC_DEV_OK = 0;
#define ESP_CODEC_DEV_MAKE_CHANNEL_MASK(channel) (1 << (channel))
struct esp_codec_dev_sample_info_t { uint32_t sample_rate; int bits_per_sample; int channel; int channel_mask; };
int esp_codec_dev_open(esp_codec_dev_handle_t, esp_codec_dev_sample_info_t *);
int esp_codec_dev_write(esp_codec_dev_handle_t, void *, int);
int esp_codec_dev_read(esp_codec_dev_handle_t, void *, int);
int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t, int);
int esp_codec_dev_set_in_gain(esp_codec_dev_handle_t, float);
void esp_codec_dev_delete(esp_codec_dev_handle_t);
