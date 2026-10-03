// SPDX-License-Identifier: MIT
// Shared I2C master bus for the Waveshare 1.85B on-board peripherals
// (touch, IMU, audio codecs, fuel gauge, RTC).

#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

namespace board_i2c {

esp_err_t init();
i2c_master_bus_handle_t bus();

// Convenience helpers for simple register-style devices.
esp_err_t readRegister(uint8_t address, uint8_t reg, uint8_t* data, size_t length);
esp_err_t writeRegister(uint8_t address, uint8_t reg, uint8_t value);
esp_err_t writeRegisters(uint8_t address, uint8_t reg, const uint8_t* data,
                         size_t length);

}  // namespace board_i2c
