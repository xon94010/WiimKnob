#include "Battery.h"
#include "Config.h"

#include <Arduino.h>

namespace power {
namespace battery {

namespace {

// A generic LiPo open-circuit-voltage discharge curve -- not specific to this battery, and it
// drifts under load and with temperature. This board has no fuel-gauge chip, so it's the best
// available without one; see BATTERY_DIVIDER_RATIO in Config.h for the other half of the
// calibration this needs.
struct CurvePoint {
  float voltage;
  int percent;
};
constexpr CurvePoint kCurve[] = {
    {4.20f, 100}, {4.06f, 90}, {3.98f, 80}, {3.92f, 70}, {3.87f, 60},
    {3.82f, 50},  {3.79f, 40}, {3.77f, 30}, {3.74f, 20}, {3.68f, 10},
    {3.45f, 5},   {3.00f, 0},
};
constexpr int kCurvePoints = sizeof(kCurve) / sizeof(kCurve[0]);

int voltageToPercent(float voltage) {
  if (voltage >= kCurve[0].voltage) {
    return 100;
  }
  if (voltage <= kCurve[kCurvePoints - 1].voltage) {
    return 0;
  }
  for (int i = 0; i < kCurvePoints - 1; i++) {
    if (voltage <= kCurve[i].voltage && voltage >= kCurve[i + 1].voltage) {
      float span = kCurve[i].voltage - kCurve[i + 1].voltage;
      float t = (voltage - kCurve[i + 1].voltage) / span;
      return kCurve[i + 1].percent + (int)(t * (kCurve[i].percent - kCurve[i + 1].percent));
    }
  }
  return 0;
}

// Measured on real hardware: raw readings jitter by as much as 90mV between samples (8x
// averaged ADC or not), so a naive per-sample delta is nearly useless -- it's noise, not
// trend. Smoothing the voltage with an EMA before looking for a trend, and still requiring
// two consecutive qualifying rises on the *smoothed* value, is what makes this usable.
constexpr float kEmaAlpha = 0.25f;
float filteredVoltage = -1;

float smooth(float rawVoltage) {
  if (filteredVoltage < 0) {
    filteredVoltage = rawVoltage; // seed on first call
  } else {
    filteredVoltage = kEmaAlpha * rawVoltage + (1.0f - kEmaAlpha) * filteredVoltage;
  }
  return filteredVoltage;
}

// Per-sample rise (samples are ~kBatteryPollMs apart, set by the caller) on the smoothed
// voltage. Two consecutive qualifying rises are required before calling it "charging", so a
// real trend takes ~2 poll intervals to be detected but a single noisy sample can't trigger a
// false positive on its own.
constexpr float kChargeRiseThreshold = 0.010f;

float prevVoltage1 = -1; // one reading ago
float prevVoltage2 = -1; // two readings ago

bool detectCharging(float voltage) {
  bool charging = false;
  if (prevVoltage1 > 0 && prevVoltage2 > 0) {
    bool risingNow = voltage > prevVoltage1 + kChargeRiseThreshold;
    bool risingBefore = prevVoltage1 > prevVoltage2 + kChargeRiseThreshold;
    charging = risingNow && risingBefore;
  }
  prevVoltage2 = prevVoltage1;
  prevVoltage1 = voltage;
  return charging;
}

} // namespace

void begin() {
  analogReadResolution(12);
  pinMode(BATTERY_ADC_PIN, INPUT);
}

Reading read() {
  constexpr int kSamples = 8;
  uint32_t sumMilliVolts = 0;
  for (int i = 0; i < kSamples; i++) {
    sumMilliVolts += analogReadMilliVolts(BATTERY_ADC_PIN);
  }
  float sensedVolts = (sumMilliVolts / (float)kSamples) / 1000.0f;
  float rawVoltage = sensedVolts * BATTERY_DIVIDER_RATIO;

  Reading r;
  r.voltage = smooth(rawVoltage);
  r.percent = voltageToPercent(r.voltage);
  r.hoursRemaining = (r.percent / 100.0f) * BATTERY_FULL_RUNTIME_HOURS;
  r.charging = detectCharging(r.voltage);
  return r;
}

} // namespace battery
} // namespace power
