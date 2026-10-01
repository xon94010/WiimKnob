#pragma once

#include <lvgl.h>

namespace ui {

struct Callbacks {
  void (*onPrev)();
  void (*onNext)();
  void (*onPlayPause)();
};

void begin(const Callbacks &callbacks);

void setTrack(const char *title, const char *artist);
void setAlbumArt(const lv_img_dsc_t *art); // nullptr shows the placeholder
void setVolume(int volume0to100); // also updates the plain-number volume badge
void setPlaying(bool playing);
void setConnected(bool connected); // small dot at the top of the controls screen

// Battery-shaped icon with a proportional, color-coded fill; tap toggles it to a plain
// percentage (tap again to go back). charging pulses it -- see the caveat on
// Reading::charging in Battery.h about how that's actually detected.
void setBattery(int percent0to100, bool charging);

} // namespace ui
