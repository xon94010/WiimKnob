#include "Touch.h"
#include "Pins.h"

#include <Arduino.h>
#include <Wire.h>

namespace input {

namespace {
constexpr uint8_t kTouchAddr = 0x15;
}

void Touch::begin() {
  Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL);

  pinMode(PIN_TOUCH_RST, OUTPUT);
  digitalWrite(PIN_TOUCH_RST, LOW);
  delay(60);
  digitalWrite(PIN_TOUCH_RST, HIGH);
  delay(60);

  pinMode(PIN_TOUCH_INT, INPUT);
}

bool Touch::read(int16_t &x, int16_t &y) {
  Wire.beginTransmission(kTouchAddr);
  Wire.write(0x02); // FingerNum register; XposH/XposL/YposH/YposL follow it
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  if (Wire.requestFrom((int)kTouchAddr, 5) != 5) {
    return false;
  }

  uint8_t fingerNum = Wire.read();
  uint8_t xposH = Wire.read();
  uint8_t xposL = Wire.read();
  uint8_t yposH = Wire.read();
  uint8_t yposL = Wire.read();

  if (fingerNum == 0) {
    return false;
  }

  int16_t rawX = ((xposH & 0x0F) << 8) | xposL;
  int16_t rawY = ((yposH & 0x0F) << 8) | yposL;

  // The panel is mounted 180 degrees rotated (MADCTL 0xC0) and the touch controller reports
  // in its own, un-rotated frame, so the coordinates need the same mirroring the pixels get.
  x = 359 - rawX;
  y = 359 - rawY;
  return true;
}

} // namespace input
