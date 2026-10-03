#include <Arduino.h>
#include <WiFi.h>
#include <lvgl.h>

#include "Config.h"
#include "display/DisplayDriver.h"
#include "haptics/Haptics.h"
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
uint32_t lastSeenPresetsVersion = 0;
uint32_t lastSeenRecentVersion = 0;
uint32_t lastRecentMetaVersion = 0;
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

void onPrev() {
  wiim::task::requestPrevious();
  haptics::buzz();
}
void onNext() {
  wiim::task::requestNext();
  haptics::buzz();
}
void onPlayPause() {
  wiim::task::requestTogglePause();
  haptics::buzz();
}
void onPreset(int key) {
  wiim::task::requestPreset(key);
  haptics::buzz();
}
void onRecent(int slot) {
  wiim::task::requestRecent(slot);
  haptics::buzz();
}

// Describes a preset by what its routine changes, since that's all the WiiM stores about it
// (no artwork): input first, then a USB DAC output, else just "EQ".
void describePreset(const wiim::Preset &p, bool wiimOnEthernet, String &subtitle, ui::PresetIcon &icon) {
  if (p.input == "wifi") {
    subtitle = wiimOnEthernet ? "ethernet" : "wi-fi"; // "wifi" here means network streaming
  } else {
    subtitle = p.input;
  }
  if (p.usbOutput) {
    // U+2022 bullet: one of the few non-ASCII glyphs the stock montserrat_14 includes. Kept
    // short -- "ethernet • USB DAC" doesn't fit a tile's width.
    subtitle += subtitle.length() ? " \xE2\x80\xA2 USB" : "USB DAC";
  }
  if (subtitle.length() == 0) {
    subtitle = p.changesEq ? "EQ" : "routine";
  }

  if (p.usbOutput) {
    icon = ui::PresetIcon::kHeadphones;
  } else if (p.input.length() && p.input != "wifi") {
    icon = ui::PresetIcon::kDisc; // a physical input: turntable, CD, ...
  } else if (p.input.length() == 0 && p.changesEq) {
    icon = ui::PresetIcon::kEq;
  } else {
    icon = ui::PresetIcon::kMusic;
  }
}

// Rebuilt when the saved list changes, and on every track change too so the red ring follows
// whichever album is actually playing.
void syncRecent() {
  uint32_t version = wiim::task::recentVersion();
  uint32_t metaVersion = wiim::task::metadataVersion();
  if (version == lastSeenRecentVersion && metaVersion == lastRecentMetaVersion) {
    return;
  }
  lastSeenRecentVersion = version;
  lastRecentMetaVersion = metaVersion;

  wiim::recent::List list = wiim::task::getRecent();
  wiim::TrackMetadata meta = wiim::task::getMetadata();
  ui::RecentTile tiles[wiim::recent::kMaxAlbums];
  for (int i = 0; i < list.count; i++) {
    const wiim::recent::Entry &e = list.items[i];
    tiles[i].id = e.slot;
    tiles[i].thumb = wiim::task::getRecentThumbnail(e.slot);
    tiles[i].playing = e.album == meta.album && e.artist == meta.artist;
  }
  ui::setRecent(tiles, list.count);
}

void syncPresets() {
  uint32_t version = wiim::task::presetsVersion();
  if (version == lastSeenPresetsVersion) {
    return;
  }
  lastSeenPresetsVersion = version;

  wiim::PresetList list = wiim::task::getPresets();
  String subtitles[wiim::kMaxPresets];
  ui::PresetTile tiles[wiim::kMaxPresets];
  for (int i = 0; i < list.count; i++) {
    const wiim::Preset &p = list.items[i];
    tiles[i].key = p.key;
    tiles[i].name = p.name.c_str();
    describePreset(p, list.wiimOnEthernet, subtitles[i], tiles[i].icon);
    tiles[i].subtitle = subtitles[i].c_str();
  }
  ui::setPresets(tiles, list.count);
}

// Screen-off double tap: toggles play/pause without waking the screen. A single tap still
// wakes it, just after the double-tap window has passed (it has to wait to see whether a
// second tap is coming). LVGL isn't running while the screen is off, so this reads the touch
// controller directly and does its own press-edge detection.
constexpr uint32_t kScreenOffDoubleTapMs = 350;
bool screenOffTouchDown = false;
uint32_t screenOffPendingTapMs = 0; // 0 = no first tap waiting

void serviceScreenOffTouch() {
  int16_t x, y;
  bool down = touch.read(x, y);
  bool pressed = down && !screenOffTouchDown;
  screenOffTouchDown = down;
  uint32_t now = millis();

  if (pressed) {
    if (screenOffPendingTapMs != 0 && now - screenOffPendingTapMs <= kScreenOffDoubleTapMs) {
      screenOffPendingTapMs = 0;
      onPlayPause(); // buzzes, so there's feedback even with the screen dark
    } else {
      screenOffPendingTapMs = now ? now : 1;
    }
    return;
  }
  if (screenOffPendingTapMs != 0 && now - screenOffPendingTapMs > kScreenOffDoubleTapMs) {
    screenOffPendingTapMs = 0;
    power::noteActivity(); // just one tap: wake the screen as before
  }
}

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
  ui::setConnected(wiim::task::isConnected());

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
  ui::flashVolume(localVolume);
  wiim::task::requestVolume(localVolume);
  if (power::isScreenOn()) {
    display::refreshNow(); // show this detent now, not on the next refresh tick
  }
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

  ui::Callbacks callbacks = {onPrev, onNext, onPlayPause, onPreset, onRecent};
  ui::begin(callbacks);

  static lv_indev_drv_t indevDrv;
  lv_indev_drv_init(&indevDrv);
  indevDrv.type = LV_INDEV_TYPE_POINTER;
  indevDrv.read_cb = touchReadCb;
  lv_indev_drv_register(&indevDrv);

  touch.begin();
  haptics::begin(); // after touch.begin(), which brings up the I2C bus haptics shares
  input::knob::begin();
  power::begin();
  power::battery::begin();

  connectWiFi();

  wiim::task::begin();
}

void loop() {
  if (power::isScreenOn()) {
    display::tick(); // pumps LVGL, which reads touch via the indev callback above
    screenOffTouchDown = true; // a finger still down as the screen times out isn't a new tap
    screenOffPendingTapMs = 0;
  } else {
    // LVGL isn't running, so nothing is calling touchReadCb -- poll directly for a wake tap or
    // a double-tap. The next loop() iteration resumes display::tick() as normal once
    // noteActivity() turns the backlight back on.
    serviceScreenOffTouch();
  }
  serviceKnob();
  syncUiFromWiimTask();
  syncPresets();
  syncRecent();
  serviceBattery();
  power::service();

  delay(5);
}
