#pragma once
#include "esp_codec_dev.h"
esp_codec_dev_handle_t bsp_audio_codec_speaker_init();
esp_codec_dev_handle_t bsp_audio_codec_microphone_init();
inline int bsp_display_brightness_set(int) { return 0; }
