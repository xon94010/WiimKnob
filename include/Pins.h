#pragma once

// Pin map for the Waveshare ESP32-S3-Knob-Touch-LCD-1.8 (ESP32-S3R8 side).
//
// Waveshare does not publish these; this list comes from the community's reverse-engineered,
// individually-verified pinout for this exact board (schematic + factory firmware + probing):
// https://github.com/teetotum-rs/firmware/blob/main/docs/hardware/pins.md
//
// The board carries a second, classic ESP32 (ESP32-U4WDH) that owns the audio DAC, the
// classic-Bluetooth stack and a second physical encoder. None of that is used here: this
// project only drives the ESP32-S3 side (display, touch, knob, Wi-Fi).

// --- Display: ST77916, 360x360 round IPS, QSPI ---
#define PIN_LCD_CS 14
#define PIN_LCD_SCK 13
#define PIN_LCD_D0 15
#define PIN_LCD_D1 16
#define PIN_LCD_D2 17
#define PIN_LCD_D3 18
#define PIN_LCD_RST 21
#define PIN_LCD_BACKLIGHT 47

// --- Touch: CST816D, I2C address 0x15 ---
#define PIN_TOUCH_SDA 11
#define PIN_TOUCH_SCL 12
#define PIN_TOUCH_INT 9
#define PIN_TOUCH_RST 10

// --- Knob: NOT a quadrature encoder. Each direction pulses its own line low
// (verified: GPIO8 low = one clockwise detent, GPIO7 low = one counter-clockwise detent).
// See docs/hardware/input.md in the repo above for how this was measured.
#define PIN_KNOB_CW 8
#define PIN_KNOB_CCW 7

// --- Haptics: DRV2605L, I2C address 0x5A, shares the touch controller's bus (SDA/SCL above).
// The chip answers on I2C whether or not this pin is driven, but its output stage stays off
// (and everything feels like "no motor attached") until this pin is held high.
#define PIN_HAPTIC_ENABLE 38
