// SPDX-License-Identifier: MIT
// Codex Micro for Waveshare ESP32-S3-Touch-LCD-1.85B
//
// Pin map taken from the official Waveshare BSP
// (Examples/ESP-IDF-V5.5.3/.../waveshare__esp32_s3_touch_lcd_1_85B).

#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"

// ---------------------------------------------------------------- display ---
#define BOARD_LCD_H_RES        (360)
#define BOARD_LCD_V_RES        (360)
#define BOARD_LCD_SPI_HOST     (SPI2_HOST)
#define BOARD_LCD_CS           (GPIO_NUM_21)
#define BOARD_LCD_PCLK         (GPIO_NUM_40)
#define BOARD_LCD_DATA0        (GPIO_NUM_46)
#define BOARD_LCD_DATA1        (GPIO_NUM_45)
#define BOARD_LCD_DATA2        (GPIO_NUM_42)
#define BOARD_LCD_DATA3        (GPIO_NUM_41)
#define BOARD_LCD_RST          (GPIO_NUM_3)
#define BOARD_LCD_BACKLIGHT    (GPIO_NUM_5)
#define BOARD_LCD_PIXEL_CLOCK_HZ (80 * 1000 * 1000)
#define BOARD_LCD_LEDC_CH      (LEDC_CHANNEL_1)

// ------------------------------------------------------------------ touch ---
#define BOARD_TOUCH_I2C_PORT   (0)
#define BOARD_I2C_SCL          (GPIO_NUM_10)
#define BOARD_I2C_SDA          (GPIO_NUM_11)
#define BOARD_TOUCH_RST        (GPIO_NUM_1)
#define BOARD_TOUCH_INT        (GPIO_NUM_4)

// ------------------------------------------------------------------ audio ---
#define BOARD_I2S_MCLK         (GPIO_NUM_2)
#define BOARD_I2S_BCLK         (GPIO_NUM_48)
#define BOARD_I2S_LRCK         (GPIO_NUM_38)
#define BOARD_I2S_DOUT         (GPIO_NUM_47)
#define BOARD_I2S_DIN          (GPIO_NUM_39)
#define BOARD_POWER_AMP        (GPIO_NUM_9)

// ---------------------------------------------------------------- buttons ---
// The 1.85B exposes only the strapping BOOT key as a user input.
#define BOARD_BUTTON_BOOT      (GPIO_NUM_0)

// ------------------------------------------------------------------ misc ----
#define BOARD_I2C_ADDR_ES8311  (0x30)
#define BOARD_I2C_ADDR_ES7210  (0x80)
#define BOARD_I2C_ADDR_BQ27220 (0x55)
#define BOARD_I2C_ADDR_CST816S (0x15)
#define BOARD_I2C_ADDR_QMI8658 (0x6B)
#define BOARD_I2C_ADDR_PCF85063 (0x51)
