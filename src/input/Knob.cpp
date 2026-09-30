#include "Knob.h"
#include "Pins.h"

#include <Arduino.h>

namespace input {
namespace knob {

namespace {

constexpr uint32_t kDebounceUs = 5000; // 5ms, matches the measured pulse width on this board

volatile int32_t cwCount = 0;
volatile int32_t ccwCount = 0;
volatile uint32_t lastCwUs = 0;
volatile uint32_t lastCcwUs = 0;

void IRAM_ATTR onCw() {
  uint32_t now = micros();
  if (now - lastCwUs > kDebounceUs) {
    cwCount = cwCount + 1;
    lastCwUs = now;
  }
}

void IRAM_ATTR onCcw() {
  uint32_t now = micros();
  if (now - lastCcwUs > kDebounceUs) {
    ccwCount = ccwCount + 1;
    lastCcwUs = now;
  }
}

} // namespace

void begin() {
  pinMode(PIN_KNOB_CW, INPUT);
  pinMode(PIN_KNOB_CCW, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_KNOB_CW), onCw, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_KNOB_CCW), onCcw, FALLING);
}

int consumeDelta() {
  noInterrupts();
  int32_t cw = cwCount;
  int32_t ccw = ccwCount;
  cwCount = 0;
  ccwCount = 0;
  interrupts();
  return (int)(cw - ccw);
}

} // namespace knob
} // namespace input
