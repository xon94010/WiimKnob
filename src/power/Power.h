#pragma once

namespace power {

// Starts the inactivity clock. Call once from setup(), after display::begin().
void begin();

// Call whenever the user does something (touch, knob turn). Resets the inactivity clock and,
// if the screen had timed out, turns the backlight back on.
void noteActivity();

// Call every loop() iteration. Turns the backlight off after SCREEN_TIMEOUT_S of inactivity,
// and puts the board into deep sleep after DEEP_SLEEP_TIMEOUT_MIN (both in Config.h). Deep
// sleep never returns -- the board reboots into setup() when a wake source fires.
void service();

// Whether the backlight is currently on. Useful for callers that want to skip work that only
// matters while something is actually visible (see main.cpp's loop(), which stops pumping
// LVGL -- and therefore stops reading touch through it -- while this is false).
bool isScreenOn();

} // namespace power
