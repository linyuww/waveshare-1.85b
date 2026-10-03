// SPDX-License-Identifier: MIT

#include "board_i2c.h"

#include <cstring>

#include "board_config.h"
#include "bsp/esp-bsp.h"
#include "esp_check.h"
#include "esp_log.h"

namespace board_i2c {
namespace {

constexpr char kTag[] = "i2c";
constexpr uint32_t kClockHz = 400 * 1000;
constexpr size_t kMaxDevices = 8;

i2c_master_bus_handle_t s_bus = nullptr;
struct DeviceSlot {
  uint8_t address = 0;
  i2c_master_dev_handle_t handle = nullptr;
};
DeviceSlot s_devices[kMaxDevices];

i2c_master_dev_handle_t deviceFor(uint8_t address) {
  for (const DeviceSlot& slot : s_devices) {
    if (slot.handle != nullptr && slot.address == address) return slot.handle;
  }
  for (DeviceSlot& slot : s_devices) {
    if (slot.handle == nullptr) {
      const i2c_device_config_t config = {
          .dev_addr_length = I2C_ADDR_BIT_LEN_7,
          .device_address = address,
          .scl_speed_hz = kClockHz,
      };
      if (i2c_master_bus_add_device(s_bus, &config, &slot.handle) != ESP_OK) {
        return nullptr;
      }
      slot.address = address;
      return slot.handle;
    }
  }
  return nullptr;
}

}  // namespace

esp_err_t init() {
  if (s_bus != nullptr) return ESP_OK;
  // The launcher BSP owns the bus already used by CST816S touch.
  s_bus = bsp_i2c_get_handle();
  return s_bus != nullptr ? ESP_OK : ESP_ERR_INVALID_STATE;
}

i2c_master_bus_handle_t bus() { return s_bus; }

esp_err_t readRegister(uint8_t address, uint8_t reg, uint8_t* data,
                       size_t length) {
  if (s_bus == nullptr) return ESP_ERR_INVALID_STATE;
  i2c_master_dev_handle_t device = deviceFor(address);
  if (device == nullptr) return ESP_ERR_NO_MEM;
  return i2c_master_transmit_receive(device, &reg, 1, data, length, 200);
}

esp_err_t writeRegister(uint8_t address, uint8_t reg, uint8_t value) {
  return writeRegisters(address, reg, &value, 1);
}

esp_err_t writeRegisters(uint8_t address, uint8_t reg, const uint8_t* data,
                         size_t length) {
  if (s_bus == nullptr) return ESP_ERR_INVALID_STATE;
  if (length + 1 > 32) return ESP_ERR_INVALID_SIZE;
  i2c_master_dev_handle_t device = deviceFor(address);
  if (device == nullptr) return ESP_ERR_NO_MEM;
  uint8_t buffer[32];
  buffer[0] = reg;
  if (length > 0 && data != nullptr) {
    memcpy(buffer + 1, data, length);
  }
  return i2c_master_transmit(device, buffer, length + 1, 200);
}

}  // namespace board_i2c
