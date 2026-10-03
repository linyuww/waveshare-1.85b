#pragma once
#include <cstdint>
#include "esp_err.h"
constexpr int GPIO_NUM_0 = 0;
constexpr int GPIO_MODE_INPUT = 0;
constexpr int GPIO_PULLUP_ENABLE = 1;
struct gpio_config_t { uint64_t pin_bit_mask; int mode; int pull_up_en; };
inline esp_err_t gpio_config(const gpio_config_t *) { return ESP_OK; }
extern bool preview_boot_pressed;
inline int gpio_get_level(int) { return preview_boot_pressed ? 0 : 1; }
