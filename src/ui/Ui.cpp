#include "Ui.h"

namespace ui {

namespace {

Callbacks callbacks;

// Red is reserved for "this is adjustable/live": the volume ring and a highlight on
// play/pause. Everything else is neutral (dark badges, white icons) per the reference look.
constexpr uint32_t kAccentColor = 0xE53935;
constexpr uint32_t kButtonColor = 0x2a2a2a;
constexpr uint32_t kBadgeColor = 0x333333;
constexpr uint32_t kBatteryGreen = 0x639922;

constexpr int kNowPlaying = 0;
constexpr int kControls = 1;
constexpr int kScreenCount = 2;

lv_obj_t *screens[kScreenCount];
int activeScreen = kNowPlaying;

// Screen 0: now playing (ambient) -- just the art, nothing else.
lv_obj_t *artImgAmbient = nullptr;

// Screen 1: controls.
lv_obj_t *artImgControls = nullptr; // lives inside a circular-clipped container, see begin()
lv_obj_t *volumeArc = nullptr;
lv_obj_t *volumeLabel = nullptr;
lv_obj_t *titleLabel = nullptr;
lv_obj_t *artistLabel = nullptr;
lv_obj_t *playPauseIcon = nullptr;
lv_obj_t *batteryBadge = nullptr;      // clickable: taps toggle icon <-> percent
lv_obj_t *batteryIconOutline = nullptr;
lv_obj_t *batteryIconNub = nullptr;
lv_obj_t *batteryFill = nullptr;
lv_obj_t *batteryPercentLabel = nullptr;
bool batteryChargingAnimRunning = false;
bool showingBatteryPercent = false;

constexpr int kArtInsetSize = 160;
constexpr int kBattIconW = 32, kBattIconH = 18;
constexpr int kBattFillMaxW = kBattIconW - 6; // inset from the outline's border

void onPrevClicked(lv_event_t *e) {
  (void)e;
  if (callbacks.onPrev) callbacks.onPrev();
}

void onNextClicked(lv_event_t *e) {
  (void)e;
  if (callbacks.onNext) callbacks.onNext();
}

void onPlayPauseClicked(lv_event_t *e) {
  (void)e;
  if (callbacks.onPlayPause) callbacks.onPlayPause();
}

void onBatteryBadgeClicked(lv_event_t *e) {
  (void)e;
  showingBatteryPercent = !showingBatteryPercent;
  if (showingBatteryPercent) {
    lv_obj_add_flag(batteryIconOutline, LV_OBJ_FLAG_HIDDEN); // also hides its batteryFill child
    lv_obj_add_flag(batteryIconNub, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(batteryPercentLabel, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_clear_flag(batteryIconOutline, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(batteryIconNub, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(batteryPercentLabel, LV_OBJ_FLAG_HIDDEN);
  }
}

void goToScreen(int index, bool forward) {
  if (index < 0 || index >= kScreenCount || index == activeScreen) {
    return;
  }
  lv_scr_load_anim(screens[index], forward ? LV_SCR_LOAD_ANIM_MOVE_LEFT : LV_SCR_LOAD_ANIM_MOVE_RIGHT,
                    220, 0, false);
  activeScreen = index;
}

void onGesture(lv_event_t *e) {
  (void)e;
  lv_indev_t *indev = lv_indev_get_act();
  if (!indev) {
    return;
  }
  lv_dir_t dir = lv_indev_get_gesture_dir(indev);
  if (dir == LV_DIR_LEFT) {
    goToScreen(activeScreen + 1, true);
  } else if (dir == LV_DIR_RIGHT) {
    goToScreen(activeScreen - 1, false);
  }
}

void makePageDots(lv_obj_t *parent, int activeIndex) {
  constexpr int kSpacing = 14;
  float mid = (kScreenCount - 1) / 2.0f;
  for (int i = 0; i < kScreenCount; i++) {
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_set_size(dot, 6, 6);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, i == activeIndex ? lv_color_hex(kAccentColor) : lv_color_hex(0x555555), 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(dot, LV_ALIGN_BOTTOM_MID, (int)((i - mid) * kSpacing), -14);
  }
}

// A small circular badge (volume level, battery gauge) -- not a playback button, so no
// gesture-bubble is added by the caller unless it asks for one.
lv_obj_t *makeBadge(lv_obj_t *parent, int size, int xOffset, int yOffset) {
  lv_obj_t *badge = lv_obj_create(parent);
  lv_obj_set_size(badge, size, size);
  lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(badge, 0, 0);
  lv_obj_set_style_bg_color(badge, lv_color_hex(kBadgeColor), 0);
  lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
  lv_obj_align(badge, LV_ALIGN_CENTER, xOffset, yOffset);
  lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
  return badge;
}

// A borderless hit target sitting on the transparent control bar -- not a visible round
// button, just an icon plus enough tappable area around it. `highlighted` draws a thin ring
// (play/pause only).
lv_obj_t *makeBarIcon(lv_obj_t *bar, int xOffset, const char *symbol, lv_event_cb_t cb, bool highlighted,
                      lv_obj_t **iconOut = nullptr) {
  lv_obj_t *hit = lv_obj_create(bar);
  lv_obj_set_size(hit, 44, 44);
  lv_obj_set_style_radius(hit, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);
  if (highlighted) {
    lv_obj_set_style_border_width(hit, 2, 0);
    lv_obj_set_style_border_color(hit, lv_color_hex(kAccentColor), 0);
  } else {
    lv_obj_set_style_border_width(hit, 0, 0);
  }
  lv_obj_align(hit, LV_ALIGN_CENTER, xOffset, 0);
  lv_obj_clear_flag(hit, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(hit, LV_OBJ_FLAG_GESTURE_BUBBLE); // a swipe starting here still changes screens
  lv_obj_add_event_cb(hit, cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *icon = lv_label_create(hit);
  lv_label_set_text(icon, symbol);
  lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(icon, lv_color_white(), 0);
  lv_obj_center(icon);
  if (iconOut) *iconOut = icon;
  return hit;
}

void setArtOn(lv_obj_t *img, const lv_img_dsc_t *art, int coverSize) {
  if (!art) {
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_img_set_src(img, art);
  // Cover the target size: zoom so the *shorter* side reaches it, cropping whatever overflows
  // the longer side (clipped by the circular panel itself, or by the inset's own clip_corner).
  int shortest = art->header.w < art->header.h ? art->header.w : art->header.h;
  if (shortest > 0) {
    lv_img_set_zoom(img, (256 * coverSize) / shortest);
  }
  lv_obj_center(img);
  lv_obj_clear_flag(img, LV_OBJ_FLAG_HIDDEN);
}

void batteryPulseAnimCb(void *var, int32_t v) { lv_obj_set_style_bg_opa((lv_obj_t *)var, v, 0); }

} // namespace

void begin(const Callbacks &cbs) {
  callbacks = cbs;

  for (int i = 0; i < kScreenCount; i++) {
    screens[i] = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screens[i], lv_color_black(), 0);
    lv_obj_clear_flag(screens[i], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(screens[i], onGesture, LV_EVENT_GESTURE, NULL);
  }

  // ---- Screen 0: now playing ----
  artImgAmbient = lv_img_create(screens[kNowPlaying]);
  lv_obj_center(artImgAmbient);
  lv_obj_add_flag(artImgAmbient, LV_OBJ_FLAG_HIDDEN);
  makePageDots(screens[kNowPlaying], kNowPlaying);

  // ---- Screen 1: controls ----
  lv_obj_t *ctrl = screens[kControls];

  lv_obj_t *artInset = lv_obj_create(ctrl);
  lv_obj_set_size(artInset, kArtInsetSize, kArtInsetSize);
  lv_obj_set_style_radius(artInset, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_clip_corner(artInset, true, 0);
  lv_obj_set_style_bg_color(artInset, lv_color_hex(0x1c1c1c), 0);
  lv_obj_set_style_border_width(artInset, 0, 0);
  lv_obj_align(artInset, LV_ALIGN_CENTER, 0, -58);
  lv_obj_clear_flag(artInset, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(artInset, LV_OBJ_FLAG_CLICKABLE);

  artImgControls = lv_img_create(artInset);
  lv_obj_center(artImgControls);
  lv_obj_add_flag(artImgControls, LV_OBJ_FLAG_HIDDEN);

  // Volume level and battery gauge flank the art at the same height as its center.
  lv_obj_t *volumeBadge = makeBadge(ctrl, 56, -130, -30);
  volumeLabel = lv_label_create(volumeBadge);
  lv_obj_set_style_text_font(volumeLabel, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(volumeLabel, lv_color_white(), 0);
  lv_obj_center(volumeLabel);
  lv_label_set_text(volumeLabel, "--");

  batteryBadge = makeBadge(ctrl, 56, 130, -30);
  lv_obj_add_flag(batteryBadge, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(batteryBadge, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(batteryBadge, onBatteryBadgeClicked, LV_EVENT_CLICKED, NULL);

  batteryIconOutline = lv_obj_create(batteryBadge);
  lv_obj_set_size(batteryIconOutline, kBattIconW, kBattIconH);
  lv_obj_set_style_radius(batteryIconOutline, 3, 0);
  lv_obj_set_style_bg_opa(batteryIconOutline, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(batteryIconOutline, 2, 0);
  lv_obj_set_style_border_color(batteryIconOutline, lv_color_hex(0xcccccc), 0);
  // The default theme puts several px of padding on every plain lv_obj_create() container,
  // which shrinks the content area lv_obj_align() positions children within. At this icon's
  // tiny scale that was clipping most of the fill bar down to a sliver -- zero it out so our
  // own inset (the fill's own x offset below) is the only one in effect.
  lv_obj_set_style_pad_all(batteryIconOutline, 0, 0);
  lv_obj_center(batteryIconOutline);
  lv_obj_clear_flag(batteryIconOutline, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(batteryIconOutline, LV_OBJ_FLAG_CLICKABLE);

  batteryIconNub = lv_obj_create(batteryBadge);
  lv_obj_set_size(batteryIconNub, 3, 8);
  lv_obj_set_style_radius(batteryIconNub, 1, 0);
  lv_obj_set_style_bg_color(batteryIconNub, lv_color_hex(0xcccccc), 0);
  lv_obj_set_style_bg_opa(batteryIconNub, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(batteryIconNub, 0, 0);
  lv_obj_align_to(batteryIconNub, batteryIconOutline, LV_ALIGN_OUT_RIGHT_MID, 0, 0);
  lv_obj_clear_flag(batteryIconNub, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(batteryIconNub, LV_OBJ_FLAG_CLICKABLE);

  batteryFill = lv_obj_create(batteryIconOutline);
  lv_obj_set_size(batteryFill, kBattFillMaxW, kBattIconH - 6);
  lv_obj_set_style_radius(batteryFill, 1, 0);
  lv_obj_set_style_border_width(batteryFill, 0, 0);
  lv_obj_set_style_bg_opa(batteryFill, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(batteryFill, lv_color_hex(kBatteryGreen), 0);
  lv_obj_align(batteryFill, LV_ALIGN_LEFT_MID, 3, 0);
  lv_obj_clear_flag(batteryFill, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(batteryFill, LV_OBJ_FLAG_CLICKABLE);

  batteryPercentLabel = lv_label_create(batteryBadge);
  lv_obj_set_style_text_font(batteryPercentLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(batteryPercentLabel, lv_color_white(), 0);
  lv_obj_center(batteryPercentLabel);
  lv_label_set_text(batteryPercentLabel, "--%");
  lv_obj_add_flag(batteryPercentLabel, LV_OBJ_FLAG_HIDDEN); // shown only after a tap

  volumeArc = lv_arc_create(ctrl);
  lv_obj_set_size(volumeArc, 356, 356);
  lv_obj_center(volumeArc);
  lv_arc_set_bg_angles(volumeArc, 0, 360);
  lv_arc_set_rotation(volumeArc, 270);
  lv_arc_set_range(volumeArc, 0, 100);
  lv_arc_set_value(volumeArc, 0);
  lv_obj_remove_style(volumeArc, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(volumeArc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(volumeArc, 10, LV_PART_MAIN);
  lv_obj_set_style_arc_width(volumeArc, 10, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(volumeArc, lv_color_hex(0x2a2a2a), LV_PART_MAIN);
  lv_obj_set_style_arc_color(volumeArc, lv_color_hex(kAccentColor), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(volumeArc, LV_OPA_TRANSP, 0);

  titleLabel = lv_label_create(ctrl);
  lv_obj_set_width(titleLabel, 260);
  lv_label_set_long_mode(titleLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_style_text_align(titleLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(titleLabel, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(titleLabel, lv_color_white(), 0);
  lv_obj_align(titleLabel, LV_ALIGN_CENTER, 0, 30);
  lv_label_set_text(titleLabel, "Not playing");

  artistLabel = lv_label_create(ctrl);
  lv_obj_set_width(artistLabel, 260);
  lv_label_set_long_mode(artistLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_style_text_align(artistLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(artistLabel, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(artistLabel, lv_color_hex(0xa0a0a0), 0);
  lv_obj_align(artistLabel, LV_ALIGN_CENTER, 0, 53);
  lv_label_set_text(artistLabel, "");

  lv_obj_t *controlBar = lv_obj_create(ctrl);
  lv_obj_set_size(controlBar, 220, 56);
  lv_obj_set_style_radius(controlBar, 28, 0);
  lv_obj_set_style_bg_color(controlBar, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(controlBar, LV_OPA_10, 0);
  lv_obj_set_style_border_width(controlBar, 0, 0);
  lv_obj_align(controlBar, LV_ALIGN_BOTTOM_MID, 0, -47);
  lv_obj_clear_flag(controlBar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(controlBar, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(controlBar, LV_OBJ_FLAG_GESTURE_BUBBLE);

  makeBarIcon(controlBar, -66, LV_SYMBOL_PREV, onPrevClicked, false);
  makeBarIcon(controlBar, 0, LV_SYMBOL_PLAY, onPlayPauseClicked, true, &playPauseIcon);
  makeBarIcon(controlBar, 66, LV_SYMBOL_NEXT, onNextClicked, false);

  makePageDots(ctrl, kControls);

  lv_scr_load(screens[kNowPlaying]);
}

void setTrack(const char *title, const char *artist) {
  lv_label_set_text(titleLabel, (title && title[0]) ? title : "Not playing");
  lv_label_set_text(artistLabel, artist ? artist : "");
}

void setAlbumArt(const lv_img_dsc_t *art) {
  setArtOn(artImgAmbient, art, 360);
  setArtOn(artImgControls, art, kArtInsetSize);
}

void setVolume(int volume0to100) {
  lv_arc_set_value(volumeArc, volume0to100);
  lv_label_set_text_fmt(volumeLabel, "%d", volume0to100);
}

void setPlaying(bool playing) {
  lv_label_set_text(playPauseIcon, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

void setBattery(int percent0to100, bool charging) {
  if (percent0to100 < 0) percent0to100 = 0;
  if (percent0to100 > 100) percent0to100 = 100;

  int fillWidth = (kBattFillMaxW * percent0to100) / 100;
  if (fillWidth < 2) fillWidth = 2; // keep a sliver visible even near-empty
  lv_obj_set_width(batteryFill, fillWidth);

  uint32_t color = kBatteryGreen;
  if (percent0to100 <= 15) {
    color = 0xE24B4A; // red
  } else if (percent0to100 <= 35) {
    color = 0xEF9F27; // amber
  }
  uint32_t litColor = charging ? 0x97C459 : color; // brighter green while charging
  lv_obj_set_style_bg_color(batteryFill, lv_color_hex(litColor), 0);

  lv_label_set_text_fmt(batteryPercentLabel, "%d%%", percent0to100);

  if (charging && !batteryChargingAnimRunning) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, batteryBadge);
    lv_anim_set_exec_cb(&a, batteryPulseAnimCb);
    lv_anim_set_values(&a, LV_OPA_40, LV_OPA_COVER);
    lv_anim_set_time(&a, 700);
    lv_anim_set_playback_time(&a, 700);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
    batteryChargingAnimRunning = true;
  } else if (!charging && batteryChargingAnimRunning) {
    lv_anim_del(batteryBadge, batteryPulseAnimCb);
    lv_obj_set_style_bg_opa(batteryBadge, LV_OPA_COVER, 0);
    batteryChargingAnimRunning = false;
  }
}

} // namespace ui
