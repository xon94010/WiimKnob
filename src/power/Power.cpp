#include "Power.h"
#include "Config.h"
#include "Pins.h"
#include "display/DisplayDriver.h"

#include <Arduino.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

namespace power {

namespace {

uint32_t lastActivityMs = 0;
bool screenOn = true;

void enterDeepSleep(const char *reason) {
  display::setBacklight(0);

  // Wake on the same GPIOs the knob and touch controller already pulse low during normal
  // operation (see Pins.h) -- no extra wiring or wake button needed. ANY_LOW: wake as soon as
  // any one of them goes low, which is what a knob detent or a touch-down event does.
  //
  // esp_sleep_enable_ext1_wakeup() only sets the wake *condition*; it doesn't configure the
  // pins themselves. Without an explicit pull-up, ESP32-S3's RTC IO can leave a wake pin
  // floating once the regular digital GPIO drivers power down for sleep, and a floating pin
  // reading low satisfies ANY_LOW immediately -- waking the chip back up within microseconds,
  // which looks exactly like the sleep never happened.
  gpio_num_t wakeGpios[] = {(gpio_num_t)PIN_KNOB_CW, (gpio_num_t)PIN_KNOB_CCW, (gpio_num_t)PIN_TOUCH_INT};
  for (gpio_num_t pin : wakeGpios) {
    rtc_gpio_pullup_en(pin);
    rtc_gpio_pulldown_dis(pin);
  }

  uint64_t wakePins = (1ULL << PIN_KNOB_CW) | (1ULL << PIN_KNOB_CCW) | (1ULL << PIN_TOUCH_INT);
  esp_sleep_enable_ext1_wakeup(wakePins, ESP_EXT1_WAKEUP_ANY_LOW);

  Serial.printf("%s, entering deep sleep. Turn the knob or touch the screen to wake.\n", reason);
  Serial.flush();

  // ext1's wake condition is level-based, checked continuously once asleep. A brief settle
  // delay here is cheap insurance against a wake pin still transitioning right as we fall
  // asleep, which would otherwise wake the chip back up within microseconds.
  delay(500);

  esp_deep_sleep_start(); // does not return; the board reboots into setup() on wake
}

} // namespace

void begin() { lastActivityMs = millis(); }

void noteActivity() {
  lastActivityMs = millis();
  if (!screenOn) {
    display::setBacklight(255);
    screenOn = true;
  }
}

void service() {
  uint32_t idleMs = millis() - lastActivityMs;

  if (screenOn && idleMs > (uint32_t)SCREEN_TIMEOUT_S * 1000) {
    display::setBacklight(0);
    screenOn = false;
  }

  if (idleMs > (uint32_t)DEEP_SLEEP_TIMEOUT_MIN * 60000UL) {
    enterDeepSleep("Idle timeout reached");
  }
}

} // namespace power
