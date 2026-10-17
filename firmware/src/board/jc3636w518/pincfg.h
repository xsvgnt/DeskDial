// DeskDial: pin assignments of the JC3636W518 board (ESP32-S3, 1.8" round
// 360x360 ST77916 QSPI panel, CST816S capacitive touch). Written for this
// project; the macro names match what the vendor driver scr_st77916.h expects.
// SPDX-License-Identifier: MIT
#pragma once

// Display (QSPI)
#define TFT_BLK 15  // backlight, PWM
#define TFT_RST 47
#define TFT_CS 10
#define TFT_SCK 9
#define TFT_SDA0 11 // QSPI data 0..3
#define TFT_SDA1 12
#define TFT_SDA2 13
#define TFT_SDA3 14

// Touch (I2C)
#define TOUCH_PIN_NUM_I2C_SCL 8
#define TOUCH_PIN_NUM_I2C_SDA 7
#define TOUCH_PIN_NUM_INT 41
#define TOUCH_PIN_NUM_RST 40
