#pragma once
inline void taskYIELD() {}
inline unsigned preview_audio_delay = 0;
inline void vTaskDelay(unsigned ticks) { preview_audio_delay += ticks; }
