#include <Arduino.h>
#include <WiFi.h>
#include <lvgl.h>

#include "Config.h"
#include "display/DisplayDriver.h"
#include "input/Knob.h"
#include "input/Touch.h"
#include "power/Battery.h"
#include "power/Power.h"
#include "ui/Ui.h"
#include "wiim/WiimTask.h"

namespace {

constexpr uint32_t kBatteryPollMs = 5000;

input::Touch touch;

int localVolume = 0;
uint32_t lastKnobActivityMs = 0;
uint32_t lastSeenMetaVersion = 0;
uint32_t lastBatteryPollMs = 0;

void touchReadCb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  (void)drv;
  int16_t x, y;
  if (touch.read(x, y)) {
    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PR;
    power::noteActivity();
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

void onPrev() { wiim::task::requestPrevious(); }
void onNext() { wiim::task::requestNext(); }
void onPlayPause() { wiim::task::requestTogglePause(); }

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Connecting to Wi-Fi '%s'...\n", WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }
  Serial.printf("\nConnected, IP: %s\n", WiFi.localIP().toString().c_str());
}

// Mirrors whatever the background WiiM task last found onto the UI. Cheap (a couple of mutex-
// protected struct copies), so it's fine to call every loop() iteration -- no polling timer of
// its own needed here, since wiim::task owns that cadence.
void syncUiFromWiimTask() {
  wiim::PlayerState state = wiim::task::getPlayerState();
  ui::setPlaying(state.status == "play");

  // Don't fight the knob: if the user just turned it, our optimistic local value is more
  // current than whatever the device reports until it catches up.
  if (millis() - lastKnobActivityMs > 1500) {
    localVolume = state.volume;
    ui::setVolume(localVolume);
  }

  uint32_t version = wiim::task::metadataVersion();
  if (version != lastSeenMetaVersion) {
    lastSeenMetaVersion = version;
    wiim::TrackMetadata meta = wiim::task::getMetadata();
    ui::setTrack(meta.title.c_str(), meta.artist.c_str());
    ui::setAlbumArt(wiim::task::getAlbumArt());
  }
}

void serviceKnob() {
  int delta = input::knob::consumeDelta();
  if (delta == 0) {
    return;
  }
  lastKnobActivityMs = millis();
  power::noteActivity();
  localVolume = constrain(localVolume + delta * VOLUME_STEP_PER_DETENT, 0, 100);
  ui::setVolume(localVolume);
  wiim::task::requestVolume(localVolume);
}

void serviceBattery() {
  uint32_t now = millis();
  if (now - lastBatteryPollMs < kBatteryPollMs) {
    return;
  }
  lastBatteryPollMs = now;
  power::battery::Reading reading = power::battery::read();
  ui::setBattery(reading.percent, reading.charging);

  // Not shown on screen anymore (see the battery screen removal) -- kept here so the raw
  // voltage is still checkable for calibrating BATTERY_DIVIDER_RATIO in Config.h.
  Serial.printf("[battery] %.2fV -> %d%%, ~%.1fh left%s\n", reading.voltage, reading.percent,
                reading.hoursRemaining, reading.charging ? " (charging)" : "");
}

} // namespace

void setup() {
  Serial.begin(115200);

  display::begin();

  ui::Callbacks callbacks = {onPrev, onNext, onPlayPause};
  ui::begin(callbacks);

  static lv_indev_drv_t indevDrv;
  lv_indev_drv_init(&indevDrv);
  indevDrv.type = LV_INDEV_TYPE_POINTER;
  indevDrv.read_cb = touchReadCb;
  lv_indev_drv_register(&indevDrv);

  touch.begin();
  input::knob::begin();
  power::begin();
  power::battery::begin();

  connectWiFi();

  wiim::task::begin();
}

void loop() {
  display::tick();
  serviceKnob();
  syncUiFromWiimTask();
  serviceBattery();
  power::service();

  delay(5);
}
