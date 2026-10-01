#pragma once

// Light haptic feedback via the board's DRV2605L LRA driver (I2C 0x5A, shared with the touch
// controller's bus). See docs/hardware/haptics.md in
// https://github.com/teetotum-rs/firmware for how the enable pin and LRA resonance were found.
namespace haptics {

// Enables the driver and configures it for LRA playback. Call once from setup(), after
// input::Touch::begin() (which brings up the shared I2C bus). Safe to call even if the chip
// doesn't respond -- buzz() just becomes a no-op in that case.
void begin();

// Fires a short, fire-and-forget click effect. Safe to call rapidly (e.g. once per knob
// detent); each call restarts the effect rather than queuing.
void buzz();

} // namespace haptics
