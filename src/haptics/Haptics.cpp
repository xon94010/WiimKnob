#include "Haptics.h"
#include "Pins.h"

#include <Arduino.h>
#include <Wire.h>

namespace haptics {

namespace {

constexpr uint8_t kAddr = 0x5A;

// DRV2605L registers used here (full map in TI's datasheet).
constexpr uint8_t kRegMode = 0x01;
constexpr uint8_t kRegLibrary = 0x03;
constexpr uint8_t kRegWaveSeq0 = 0x04;
constexpr uint8_t kRegWaveSeq1 = 0x05;
constexpr uint8_t kRegGo = 0x0C;
constexpr uint8_t kRegFeedbackControl = 0x16;

constexpr uint8_t kModeInternalTrigger = 0x00; // clears the power-on-reset STANDBY bit
constexpr uint8_t kLibraryLra = 6;
constexpr uint8_t kEffectStrongClick = 1; // ROM effect #1, short and strong -- good for a "detent" feel
constexpr uint8_t kFeedbackLraSelect = 0x80; // bit 7: 1 = LRA (this board's actuator, not ERM)

bool chipPresent = false;

bool writeReg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(kAddr);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

} // namespace

void begin() {
  pinMode(PIN_HAPTIC_ENABLE, OUTPUT);
  digitalWrite(PIN_HAPTIC_ENABLE, HIGH); // without this the chip talks but the output stays off

  // Exit power-on standby, select the LRA effect library, and switch the feedback loop to
  // LRA mode. Deliberately not touching calibration registers (rated voltage, OD clamp,
  // auto-cal) -- the defaults produce a felt effect on this board without them, and getting
  // those wrong is how you end up with a device that's silently over-driving the actuator.
  bool ok = true;
  ok &= writeReg(kRegMode, kModeInternalTrigger);
  ok &= writeReg(kRegLibrary, kLibraryLra);
  ok &= writeReg(kRegFeedbackControl, kFeedbackLraSelect);
  chipPresent = ok;
}

void buzz() {
  if (!chipPresent) {
    return;
  }
  writeReg(kRegWaveSeq0, kEffectStrongClick);
  writeReg(kRegWaveSeq1, 0); // terminates the sequence after one effect
  writeReg(kRegGo, 1);       // GO auto-clears itself once the effect finishes playing
}

} // namespace haptics
