#pragma once

#include <lvgl.h>

namespace display {

// Bring up the QSPI bus, the ST77916 panel and LVGL's display driver. Must be called once
// from setup(), before any UI code runs.
void begin();

// Pump LVGL's timer handler. Call every loop() iteration.
void tick();

// Draws pending UI changes right away instead of on LVGL's next refresh tick -- for input that
// should show up with no extra delay (knob turns).
void refreshNow();

// Backlight brightness, 0-255.
void setBacklight(uint8_t brightness);

} // namespace display
