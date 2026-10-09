#pragma once

#include <stdint.h>

// Fixed LILYGO T_Connect_Pro_V1_0 wiring and LCD layout.
// Deployment settings belong in config.h.
#define IIC_SDA       39
#define IIC_SCL       40

#define SCREEN_WIDTH  222
#define SCREEN_HEIGHT 480
#define SCREEN_BL     46
#define SCREEN_MOSI   11
#define SCREEN_MISO   13
#define SCREEN_SCLK   12
#define SCREEN_CS     21
#define SCREEN_DC     41
#define SCREEN_RST    -1

#define TOUCH_SDA     IIC_SDA
#define TOUCH_SCL     IIC_SCL
#define TOUCH_RST     47
#define TOUCH_INT     3

#define SX1262_CS     14
#define SX1262_RST    42
#define SX1262_SCLK   12
#define SX1262_MOSI   11
#define SX1262_MISO   13
#define SX1262_BUSY   38
#define SX1262_INT    45
#define SX1262_DIO1   45

#define RELAY_1       8

constexpr uint8_t DISPLAY_ROTATION = 3;
constexpr int16_t DISPLAY_LANDSCAPE_WIDTH  = SCREEN_HEIGHT;
constexpr int16_t DISPLAY_LANDSCAPE_HEIGHT = SCREEN_WIDTH;
