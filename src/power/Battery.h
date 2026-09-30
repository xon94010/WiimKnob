#pragma once

namespace power {
namespace battery {

struct Reading {
  float voltage = 0;        // at the battery, after undoing the sense divider
  int percent = 0;          // 0-100, from a generic LiPo discharge curve
  float hoursRemaining = 0; // rough estimate; see BATTERY_FULL_RUNTIME_HOURS in Config.h

  // There's no charger-status pin on this board (confirmed unavailable in the community
  // pinout research this project relies on -- see README), so this is a software guess: two
  // consecutive readings that each rose more than sensor noise, not a real "is charging"
  // signal. It can occasionally misfire (e.g. briefly true after a load spike recovers).
  bool charging = false;
};

// Configures the ADC pin. Call once from setup().
void begin();

// Takes a fresh reading (averages a few samples internally). Cheap enough to call every few
// seconds; no need to cache it yourself. Uses the two previous readings to guess `charging`,
// so call this on a roughly steady interval (it's meant to be polled, not called back-to-back).
Reading read();

} // namespace battery
} // namespace power
