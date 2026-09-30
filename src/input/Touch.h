#pragma once

#include <stdint.h>

namespace input {

// Minimal polling driver for the CST816D capacitive touch controller (I2C address 0x15).
//
// This talks to the chip directly instead of pulling in a library, because the touch INT pin
// on this board pulses rather than staying asserted for the length of a touch -- polling the
// contact registers is the reliable read here, the interrupt is at best a hint. See
// https://github.com/teetotum-rs/firmware/blob/main/docs/hardware/input.md
class Touch {
public:
  void begin();

  // Returns true if a finger is down, and fills x/y in panel (i.e. on-screen, already
  // mount-corrected) coordinates, 0-359.
  bool read(int16_t &x, int16_t &y);
};

} // namespace input
