#include "Ui.h"

// Generated from Montserrat-Medium.ttf + the same FontAwesome symbol subset LVGL's own
// built-in fonts use, but with the main range extended to 0x20-0x7F,0xA0-0x17F,0x2022 instead
// of just ASCII -- see src/ui/fonts/ for the command. The stock lv_font_montserrat_16/20 only
// cover ASCII plus the icon symbols, so any accented character (e.g. the O with diaeresis in
// "Magnus Ostrom") had no glyph to draw at all. Only used where real track/artist text shows
// up; the icon buttons and the volume number stay on the stock fonts.
LV_FONT_DECLARE(lv_font_montserrat_20_latin);
LV_FONT_DECLARE(lv_font_montserrat_16_latin);
// Just the four FontAwesome glyphs the preset tiles use -- see the header of the .c file.
LV_FONT_DECLARE(lv_font_preset_icons_26);

namespace ui {

namespace {

Callbacks callbacks;

// Red is reserved for "this is adjustable/live": the volume ring, plus the "last tapped"
// markers on the presets/recent screens. Everything else is neutral (dark badges, white icons)
// per the reference look.
constexpr uint32_t kAccentColor = 0xE53935;
constexpr uint32_t kButtonColor = 0x2a2a2a;
constexpr uint32_t kBadgeColor = 0x333333;
constexpr uint32_t kBatteryGreen = 0x639922;

// Swipe order, left to right. Boots on the controls screen.
constexpr int kControls = 0;
constexpr int kNowPlaying = 1;
constexpr int kRecent = 2;
constexpr int kPresets = 3;
constexpr int kScreenCount = 4;

lv_obj_t *screens[kScreenCount];
int activeScreen = kControls;

// Now playing (ambient) -- just the art, nothing else.
lv_obj_t *artImgAmbient = nullptr;

// Controls.
lv_obj_t *artImgControls = nullptr; // lives inside a circular-clipped container, see begin()
lv_obj_t *volumeArc = nullptr;
lv_obj_t *volumeLabel = nullptr;
lv_obj_t *titleLabel = nullptr;
lv_obj_t *artistLabel = nullptr;
lv_obj_t *playPauseIcon = nullptr;
lv_obj_t *statusDot = nullptr;
lv_obj_t *batteryBadge = nullptr;      // clickable: taps toggle icon <-> percent
lv_obj_t *batteryIconOutline = nullptr;
lv_obj_t *batteryIconNub = nullptr;
lv_obj_t *batteryFill = nullptr;
lv_obj_t *batteryPercentLabel = nullptr;
bool batteryChargingAnimRunning = false;
bool showingBatteryPercent = false;

// Recently played.
constexpr int kMaxRecentTiles = 10;
constexpr int kRecentThumbSize = 68;
lv_obj_t *recentTiles[kMaxRecentTiles] = {};
int recentTileCount = 0;
lv_obj_t *recentEmptyLabel = nullptr;

// Presets.
lv_obj_t *presetGrid = nullptr;
lv_obj_t *presetEmptyLabel = nullptr;
lv_obj_t *lastFiredTile = nullptr; // red border: last preset fired from here, not "active" --
                                   // the WiiM doesn't report which preset is currently in effect
int lastFiredKey = 0;               // survives a grid rebuild

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
// button, just an icon plus enough tappable area around it.
lv_obj_t *makeBarIcon(lv_obj_t *bar, int xOffset, const char *symbol, lv_event_cb_t cb,
                      lv_obj_t **iconOut = nullptr) {
  lv_obj_t *hit = lv_obj_create(bar);
  lv_obj_set_size(hit, 44, 44);
  lv_obj_set_style_radius(hit, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(hit, 0, 0);
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

void markLastFired(lv_obj_t *tile) {
  if (lastFiredTile) {
    lv_obj_set_style_border_color(lastFiredTile, lv_color_black(), 0);
    lv_obj_set_style_border_opa(lastFiredTile, LV_OPA_TRANSP, 0);
  }
  lastFiredTile = tile;
  if (tile) {
    lv_obj_set_style_border_color(tile, lv_color_hex(kAccentColor), 0);
    lv_obj_set_style_border_opa(tile, LV_OPA_COVER, 0);
  }
}

void onPresetClicked(lv_event_t *e) {
  // LVGL still sends CLICKED on release after a swipe that started on a tile -- don't let
  // swiping between screens fire a preset (which can switch inputs/outputs).
  lv_indev_t *indev = lv_indev_get_act();
  if (indev && lv_indev_get_gesture_dir(indev) != LV_DIR_NONE) {
    return;
  }
  lv_obj_t *tile = lv_event_get_current_target(e);
  int key = (int)(intptr_t)lv_event_get_user_data(e);
  lastFiredKey = key;
  markLastFired(tile);
  if (callbacks.onPreset) callbacks.onPreset(key);
}

void setRecentRing(lv_obj_t *tile, bool on) {
  lv_obj_set_style_outline_opa(tile, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

void onRecentClicked(lv_event_t *e) {
  lv_indev_t *indev = lv_indev_get_act();
  if (indev && lv_indev_get_gesture_dir(indev) != LV_DIR_NONE) {
    return; // the tail end of a swipe, not a tap -- see onPresetClicked
  }
  lv_obj_t *tile = lv_event_get_current_target(e);
  // Move the ring right away; the real "now playing" state catches up once the WiiM reports it.
  for (int i = 0; i < recentTileCount; i++) {
    setRecentRing(recentTiles[i], recentTiles[i] == tile);
  }
  if (callbacks.onRecent) callbacks.onRecent((int)(intptr_t)lv_event_get_user_data(e));
}

const char *presetIconGlyph(PresetIcon icon) {
  switch (icon) {
  case PresetIcon::kDisc:
    return "\xEF\x94\x9F"; // U+F51F compact-disc
  case PresetIcon::kHeadphones:
    return "\xEF\x80\xA5"; // U+F025 headphones
  case PresetIcon::kEq:
    return "\xEF\x87\x9E"; // U+F1DE sliders-h
  case PresetIcon::kMusic:
  default:
    return "\xEF\x80\x81"; // U+F001 music
  }
}

lv_obj_t *makePresetTile(lv_obj_t *grid, const PresetTile &p) {
  lv_obj_t *tile = lv_obj_create(grid);
  lv_obj_set_size(tile, 120, 104);
  lv_obj_set_style_radius(tile, 16, 0);
  lv_obj_set_style_bg_color(tile, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(tile, LV_OPA_10, 0);
  lv_obj_set_style_bg_opa(tile, LV_OPA_20, LV_STATE_PRESSED);
  lv_obj_set_style_border_width(tile, 2, 0);
  lv_obj_set_style_border_opa(tile, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(tile, 4, 0);
  lv_obj_set_style_pad_row(tile, 2, 0);
  lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(tile, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(tile, onPresetClicked, LV_EVENT_CLICKED, (void *)(intptr_t)p.key);

  lv_obj_t *icon = lv_label_create(tile);
  lv_label_set_text(icon, presetIconGlyph(p.icon));
  lv_obj_set_style_text_font(icon, &lv_font_preset_icons_26, 0);
  lv_obj_set_style_text_color(icon, lv_color_white(), 0);

  lv_obj_t *name = lv_label_create(tile);
  lv_obj_set_width(name, 110);
  lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP); // "Headphone Mode" needs two lines
  lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(name, &lv_font_montserrat_16_latin, 0);
  lv_obj_set_style_text_color(name, lv_color_white(), 0);
  lv_label_set_text(name, p.name);

  lv_obj_t *sub = lv_label_create(tile);
  lv_obj_set_width(sub, 110);
  lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(sub, lv_color_hex(0xb4b2a9), 0);
  lv_label_set_text(sub, p.subtitle);

  if (p.key == lastFiredKey) markLastFired(tile);
  return tile;
}

} // namespace

void begin(const Callbacks &cbs) {
  callbacks = cbs;

  for (int i = 0; i < kScreenCount; i++) {
    screens[i] = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screens[i], lv_color_black(), 0);
    lv_obj_clear_flag(screens[i], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(screens[i], onGesture, LV_EVENT_GESTURE, NULL);
  }

  // ---- Now playing ----
  artImgAmbient = lv_img_create(screens[kNowPlaying]);
  lv_obj_center(artImgAmbient);
  lv_obj_add_flag(artImgAmbient, LV_OBJ_FLAG_HIDDEN);
  makePageDots(screens[kNowPlaying], kNowPlaying);

  // ---- Controls ----
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

  // Small connectivity dot above the art: green once the last WiiM poll succeeded, red if not.
  lv_obj_t *dotBacking = lv_obj_create(ctrl);
  lv_obj_set_size(dotBacking, 20, 20);
  lv_obj_set_style_radius(dotBacking, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(dotBacking, 0, 0);
  lv_obj_set_style_bg_color(dotBacking, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(dotBacking, LV_OPA_50, 0);
  lv_obj_align(dotBacking, LV_ALIGN_CENTER, 0, -150);
  lv_obj_clear_flag(dotBacking, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(dotBacking, LV_OBJ_FLAG_CLICKABLE);

  statusDot = lv_obj_create(ctrl);
  lv_obj_set_size(statusDot, 10, 10);
  lv_obj_set_style_radius(statusDot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(statusDot, 0, 0);
  lv_obj_set_style_bg_color(statusDot, lv_palette_main(LV_PALETTE_RED), 0);
  lv_obj_align(statusDot, LV_ALIGN_CENTER, 0, -150);
  lv_obj_clear_flag(statusDot, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(statusDot, LV_OBJ_FLAG_CLICKABLE);

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
  lv_obj_set_style_text_font(titleLabel, &lv_font_montserrat_20_latin, 0);
  lv_obj_set_style_text_color(titleLabel, lv_color_white(), 0);
  lv_obj_align(titleLabel, LV_ALIGN_CENTER, 0, 30);
  lv_label_set_text(titleLabel, "Not playing");

  artistLabel = lv_label_create(ctrl);
  lv_obj_set_width(artistLabel, 260);
  lv_label_set_long_mode(artistLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_style_text_align(artistLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(artistLabel, &lv_font_montserrat_16_latin, 0);
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

  makeBarIcon(controlBar, -66, LV_SYMBOL_PREV, onPrevClicked);
  makeBarIcon(controlBar, 0, LV_SYMBOL_PLAY, onPlayPauseClicked, &playPauseIcon);
  makeBarIcon(controlBar, 66, LV_SYMBOL_NEXT, onNextClicked);

  makePageDots(ctrl, kControls);

  // ---- Recently played ----
  lv_obj_t *rec = screens[kRecent];

  lv_obj_t *recentTitle = lv_label_create(rec);
  lv_obj_set_style_text_font(recentTitle, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(recentTitle, lv_color_hex(0xb4b2a9), 0);
  lv_label_set_text(recentTitle, "Recently played");
  lv_obj_align(recentTitle, LV_ALIGN_TOP_MID, 0, 46);

  lv_obj_t *recentHint = lv_label_create(rec);
  lv_obj_set_style_text_font(recentHint, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(recentHint, lv_color_hex(0x707070), 0);
  lv_label_set_text(recentHint, "Tap to play");
  lv_obj_align(recentHint, LV_ALIGN_BOTTOM_MID, 0, -50);

  recentEmptyLabel = lv_label_create(rec);
  lv_obj_set_width(recentEmptyLabel, 220);
  lv_label_set_long_mode(recentEmptyLabel, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(recentEmptyLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(recentEmptyLabel, &lv_font_montserrat_16_latin, 0);
  lv_obj_set_style_text_color(recentEmptyLabel, lv_color_hex(0xa0a0a0), 0);
  lv_label_set_text(recentEmptyLabel, "Albums you play will show up here");
  lv_obj_center(recentEmptyLabel);

  makePageDots(rec, kRecent);

  // ---- Presets ----
  lv_obj_t *pres = screens[kPresets];

  lv_obj_t *presetsTitle = lv_label_create(pres);
  lv_obj_set_style_text_font(presetsTitle, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(presetsTitle, lv_color_hex(0xb4b2a9), 0);
  lv_label_set_text(presetsTitle, "Presets");
  lv_obj_align(presetsTitle, LV_ALIGN_TOP_MID, 0, 34);

  // Two columns of tiles; scrolls vertically if more than four presets are set up. Only
  // vertical scrolling, so a horizontal swipe still reaches the screen's gesture handler.
  presetGrid = lv_obj_create(pres);
  lv_obj_set_size(presetGrid, 254, 222);
  lv_obj_align(presetGrid, LV_ALIGN_TOP_MID, 0, 62);
  lv_obj_set_style_bg_opa(presetGrid, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(presetGrid, 0, 0);
  lv_obj_set_style_pad_all(presetGrid, 2, 0);
  lv_obj_set_style_pad_gap(presetGrid, 10, 0);
  lv_obj_set_flex_flow(presetGrid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(presetGrid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_scroll_dir(presetGrid, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(presetGrid, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_flag(presetGrid, LV_OBJ_FLAG_GESTURE_BUBBLE);

  presetEmptyLabel = lv_label_create(pres);
  lv_obj_set_width(presetEmptyLabel, 220);
  lv_label_set_long_mode(presetEmptyLabel, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(presetEmptyLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(presetEmptyLabel, &lv_font_montserrat_16_latin, 0);
  lv_obj_set_style_text_color(presetEmptyLabel, lv_color_hex(0xa0a0a0), 0);
  lv_label_set_text(presetEmptyLabel, "Loading presets...");
  lv_obj_center(presetEmptyLabel);

  makePageDots(pres, kPresets);

  lv_scr_load(screens[kControls]);
}

void setRecent(const RecentTile *tiles, int count) {
  // Rows of 3-4-3 centered on the circle; offsets from the screen center.
  static const int8_t kPos[kMaxRecentTiles][2] = {
      {-78, -72}, {0, -72}, {78, -72},
      {-117, 0}, {-39, 0}, {39, 0}, {117, 0},
      {-78, 72}, {0, 72}, {78, 72},
  };
  if (count > kMaxRecentTiles) count = kMaxRecentTiles;

  for (int i = 0; i < recentTileCount; i++) {
    lv_obj_del(recentTiles[i]);
    recentTiles[i] = nullptr;
  }
  recentTileCount = count;

  for (int i = 0; i < count; i++) {
    lv_obj_t *tile = lv_obj_create(screens[kRecent]);
    lv_obj_set_size(tile, kRecentThumbSize, kRecentThumbSize);
    lv_obj_set_style_radius(tile, 12, 0);
    lv_obj_set_style_clip_corner(tile, true, 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_style_pad_all(tile, 0, 0);
    lv_obj_set_style_outline_width(tile, 2, 0);
    lv_obj_set_style_outline_pad(tile, 2, 0);
    lv_obj_set_style_outline_color(tile, lv_color_hex(kAccentColor), 0);
    setRecentRing(tile, tiles[i].playing);
    lv_obj_align(tile, LV_ALIGN_CENTER, kPos[i][0], kPos[i][1]);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(tile, onRecentClicked, LV_EVENT_CLICKED, (void *)(intptr_t)tiles[i].id);

    if (tiles[i].thumb) {
      lv_obj_t *img = lv_img_create(tile);
      lv_img_set_src(img, tiles[i].thumb);
      lv_obj_center(img);
    }
    recentTiles[i] = tile;
  }

  if (count == 0) {
    lv_obj_clear_flag(recentEmptyLabel, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(recentEmptyLabel, LV_OBJ_FLAG_HIDDEN);
  }
}

void setPresets(const PresetTile *tiles, int count) {
  lastFiredTile = nullptr; // about to be deleted with the rest of the grid's children
  lv_obj_clean(presetGrid);
  for (int i = 0; i < count; i++) {
    makePresetTile(presetGrid, tiles[i]);
  }
  // Only scrollable when the tiles don't all fit. A scrollable container starts scrolling on
  // ~10px of finger drift even with nothing to scroll to, and a scroll swallows the tap --
  // which made taps on the touchscreen hit-or-miss.
  if (count > 4) {
    lv_obj_add_flag(presetGrid, LV_OBJ_FLAG_SCROLLABLE);
  } else {
    lv_obj_clear_flag(presetGrid, LV_OBJ_FLAG_SCROLLABLE);
  }
  if (count == 0) {
    lv_label_set_text(presetEmptyLabel, "No presets set up in the WiiM Home app");
    lv_obj_clear_flag(presetEmptyLabel, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(presetEmptyLabel, LV_OBJ_FLAG_HIDDEN);
  }
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

void setConnected(bool connected) {
  lv_obj_set_style_bg_color(statusDot,
                             connected ? lv_palette_main(LV_PALETTE_GREEN) : lv_palette_main(LV_PALETTE_RED), 0);
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
