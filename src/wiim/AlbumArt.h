#pragma once

#include <Arduino.h>
#include <lvgl.h>

namespace wiim {

// Downloads a cover-art JPEG (from whatever external host the WiiM's albumArtURI points at)
// and decodes it into an LVGL image descriptor. Keeps one decode buffer around and only
// re-fetches when the URL actually changes -- callers should call fetch() unconditionally on
// every metadata poll and just check the return value / whether the descriptor changed.
namespace albumart {

// Downloads and decodes `url`. No-op (returns true immediately) if it matches the
// already-decoded image. Returns false on network or decode failure -- the previous
// descriptor, if any, is left in place.
bool fetch(const String &url);

// The most recently decoded image, or nullptr if none has been fetched yet.
const lv_img_dsc_t *descriptor();

// URL of the image descriptor() currently holds (empty if none).
const String &currentUrl();

// Box-filters a center square crop of the current image down to size x size RGB565 into dst.
// Returns false if there's no image yet. Call from the same task as fetch().
bool makeThumbnail(uint16_t *dst, int size);

} // namespace albumart
} // namespace wiim
