#include "Knob.h"
#include "Pins.h"

#include <Arduino.h>
#include <esp_timer.h>

namespace input {
namespace knob {

namespace {

// Sampled rather than edge-triggered. Counting falling edges (the first version of this file)
// lost most detents on a fast spin, measured on-device: the edges ring, so one pulse produced
// several falling edges, and both lines dip together for a moment now and then -- which counted
// as one clockwise plus one counter-clockwise detent that cancelled out. The community
// teardown this pinout comes from saw the same thing: edge counting gave 37-41 per revolution,
// polling the pin levels exactly 30.
//
// So: sample both pins every 1ms, only trust a level once it has held for kStableSamples
// samples, and count a detent the first time the settled state has exactly one line low after
// being idle (both high). Both-low is ignored, and the next detent needs a return to idle.
constexpr uint64_t kSamplePeriodUs = 1000;
constexpr int kStableSamples = 2; // pulses are 15-30ms low at hand speed, a few ms at a fast spin

// Bit 1 = CW line (GPIO8), bit 0 = CCW line (GPIO7). High = idle.
constexpr uint8_t kIdle = 0b11;
constexpr uint8_t kCwPulse = 0b01;  // GPIO8 low, GPIO7 high
constexpr uint8_t kCcwPulse = 0b10; // GPIO7 low, GPIO8 high

esp_timer_handle_t sampleTimer = nullptr;
portMUX_TYPE countLock = portMUX_INITIALIZER_UNLOCKED;
int32_t pendingDelta = 0;

uint8_t candidate = kIdle;
int candidateCount = 0;
uint8_t settled = kIdle;
bool armed = true; // false after a detent until the knob settles back to idle

void sample(void *) {
  uint8_t now = (uint8_t)((digitalRead(PIN_KNOB_CW) ? 0b10 : 0) | (digitalRead(PIN_KNOB_CCW) ? 0b01 : 0));
  if (now != candidate) {
    candidate = now;
    candidateCount = 1;
    return;
  }
  if (candidateCount < kStableSamples) {
    candidateCount++;
    if (candidateCount < kStableSamples) return;
  } else {
    return; // already settled on this state
  }

  // `candidate` just became the settled state.
  settled = candidate;
  if (settled == kIdle) {
    armed = true;
    return;
  }
  if (settled == 0b00) {
    return;
  }
  if (!armed) {
    return; // still the same pulse (e.g. it went one-low -> both-low -> one-low)
  }
  armed = false;
  int step = settled == kCwPulse ? 1 : -1;
  portENTER_CRITICAL(&countLock);
  pendingDelta += step;
  portEXIT_CRITICAL(&countLock);
}

} // namespace

void begin() {
  pinMode(PIN_KNOB_CW, INPUT);
  pinMode(PIN_KNOB_CCW, INPUT);
  esp_timer_create_args_t args = {};
  args.callback = sample;
  args.name = "knob";
  esp_timer_create(&args, &sampleTimer);
  esp_timer_start_periodic(sampleTimer, kSamplePeriodUs);
}

int consumeDelta() {
  portENTER_CRITICAL(&countLock);
  int32_t d = pendingDelta;
  pendingDelta = 0;
  portEXIT_CRITICAL(&countLock);
  return (int)d;
}

} // namespace knob
} // namespace input
