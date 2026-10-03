#include "AlbumArt.h"

#include <HTTPClient.h>
#include <JPEGDEC.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>

namespace wiim {
namespace albumart {

namespace {

constexpr size_t kMaxJpegBytes = 400 * 1024;

uint8_t *rawBuf = nullptr;
size_t rawCap = 0;
size_t rawLen = 0;

uint16_t *pixelBuf = nullptr;
size_t pixelCap = 0; // bytes

// Only allocated/used when the decoded image is bigger than kWorkingSize -- see
// boxDownscale() below for why this exists instead of just letting LVGL's zoom handle it.
uint16_t *smallBuf = nullptr;
size_t smallCap = 0; // bytes
// Shorter side of the final image: the full screen width, so the full-screen art page draws it
// 1:1. Anything else means LVGL scales it in software on every redraw -- slow enough over a
// full-screen image that a big volume readout on top of it couldn't keep up with fast knob
// turns. The small circular inset on the controls page still scales it down, but over far
// fewer pixels.
constexpr int kWorkingSize = 360;

lv_img_dsc_t desc = {};
String lastUrl;

JPEGDEC jpeg;

// Set just before decode() and read from the (synchronous, single-threaded) draw callback.
uint16_t *decodeTarget = nullptr;
int decodeTargetW = 0;
int decodeTargetH = 0;

int drawCallback(JPEGDRAW *pDraw) {
  if (!decodeTarget) {
    return 0;
  }
  for (int row = 0; row < pDraw->iHeight; row++) {
    int destY = pDraw->y + row;
    if (destY < 0 || destY >= decodeTargetH) {
      continue;
    }
    int destX = pDraw->x;
    int copyW = pDraw->iWidthUsed;
    if (destX < 0 || destX >= decodeTargetW) {
      continue;
    }
    if (destX + copyW > decodeTargetW) {
      copyW = decodeTargetW - destX;
    }
    memcpy(&decodeTarget[destY * decodeTargetW + destX], &pDraw->pPixels[row * pDraw->iWidth], copyW * 2);
  }
  return 1;
}

bool ensureRawCap(size_t needed) {
  if (needed <= rawCap) {
    return true;
  }
  if (rawBuf) {
    heap_caps_free(rawBuf);
  }
  rawBuf = (uint8_t *)heap_caps_malloc(needed, MALLOC_CAP_SPIRAM);
  rawCap = rawBuf ? needed : 0;
  return rawBuf != nullptr;
}

bool ensurePixelCap(size_t neededBytes) {
  if (neededBytes <= pixelCap) {
    return true;
  }
  if (pixelBuf) {
    heap_caps_free(pixelBuf);
  }
  pixelBuf = (uint16_t *)heap_caps_malloc(neededBytes, MALLOC_CAP_SPIRAM);
  pixelCap = pixelBuf ? neededBytes : 0;
  return pixelBuf != nullptr;
}

bool ensureSmallCap(size_t neededBytes) {
  if (neededBytes <= smallCap) {
    return true;
  }
  if (smallBuf) {
    heap_caps_free(smallBuf);
  }
  smallBuf = (uint16_t *)heap_caps_malloc(neededBytes, MALLOC_CAP_SPIRAM);
  smallCap = smallBuf ? neededBytes : 0;
  return smallBuf != nullptr;
}

// JPEGDEC's own scaling only comes in powers of two, so the decoded image is usually still
// noticeably bigger than where it ends up on screen (particularly the circular art inset,
// 160px). Handing that gap to LVGL's zoom means nearest-neighbor downscaling, which aliases
// fine detail into a shimmery mess. A box filter -- averaging the block of source pixels each
// destination pixel covers -- looks meaningfully sharper for the same cost of one extra pass
// over the source (each source pixel is visited once, no more).
// srcStride is the source's full row width in pixels, so this can also downscale a crop of it.
void boxDownscale(const uint16_t *src, int srcStride, int srcW, int srcH, uint16_t *dst, int dstW, int dstH) {
  for (int dy = 0; dy < dstH; dy++) {
    int sy0 = dy * srcH / dstH;
    int sy1 = (dy + 1) * srcH / dstH;
    if (sy1 <= sy0) sy1 = sy0 + 1;
    for (int dx = 0; dx < dstW; dx++) {
      int sx0 = dx * srcW / dstW;
      int sx1 = (dx + 1) * srcW / dstW;
      if (sx1 <= sx0) sx1 = sx0 + 1;

      uint32_t rSum = 0, gSum = 0, bSum = 0, count = 0;
      for (int sy = sy0; sy < sy1; sy++) {
        const uint16_t *row = &src[sy * srcStride];
        for (int sx = sx0; sx < sx1; sx++) {
          uint16_t px = row[sx];
          rSum += (px >> 11) & 0x1F;
          gSum += (px >> 5) & 0x3F;
          bSum += px & 0x1F;
          count++;
        }
      }
      uint16_t r = rSum / count, g = gSum / count, b = bSum / count;
      dst[dy * dstW + dx] = (uint16_t)((r << 11) | (g << 5) | b);
    }
  }
}

} // namespace

bool fetch(const String &url) {
  if (url.length() == 0) {
    return false;
  }
  if (url == lastUrl && desc.data != nullptr) {
    return true; // already showing this image
  }

  WiFiClientSecure client;
  client.setInsecure(); // arbitrary external art host; we only display the bytes, no secrets involved

  HTTPClient http;
  if (!http.begin(client, url)) {
    return false;
  }
  http.setTimeout(5000);

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  if (!ensureRawCap(kMaxJpegBytes)) {
    http.end();
    return false;
  }

  NetworkClient *stream = http.getStreamPtr();
  rawLen = 0;
  uint32_t lastByteAt = millis();
  while (http.connected() && rawLen < kMaxJpegBytes) {
    size_t avail = stream->available();
    if (avail == 0) {
      if (!stream->connected() || millis() - lastByteAt > 5000) {
        break;
      }
      delay(2);
      continue;
    }
    size_t toRead = min(avail, kMaxJpegBytes - rawLen);
    int got = stream->readBytes(rawBuf + rawLen, toRead);
    if (got <= 0) {
      break;
    }
    rawLen += got;
    lastByteAt = millis();
  }
  http.end();

  if (rawLen == 0) {
    return false;
  }

  if (jpeg.openRAM(rawBuf, rawLen, drawCallback) != 1) {
    return false;
  }

  int srcW = jpeg.getWidth();
  int srcH = jpeg.getHeight();

  // Biggest power-of-two JPEG downscale that still leaves the shorter side at least
  // kWorkingSize, so the box filter below finishes the job without ever upscaling.
  int divisor = 1;
  int scaleFlag = 0;
  int shortest = min(srcW, srcH);
  if (shortest / 8 >= kWorkingSize) {
    divisor = 8;
    scaleFlag = JPEG_SCALE_EIGHTH;
  } else if (shortest / 4 >= kWorkingSize) {
    divisor = 4;
    scaleFlag = JPEG_SCALE_QUARTER;
  } else if (shortest / 2 >= kWorkingSize) {
    divisor = 2;
    scaleFlag = JPEG_SCALE_HALF;
  }

  int outW = srcW / divisor;
  int outH = srcH / divisor;
  size_t neededBytes = (size_t)outW * outH * 2;

  if (!ensurePixelCap(neededBytes)) {
    jpeg.close();
    return false;
  }

  decodeTarget = pixelBuf;
  decodeTargetW = outW;
  decodeTargetH = outH;

  jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
  bool ok = jpeg.decode(0, 0, scaleFlag) == 1;
  jpeg.close();
  decodeTarget = nullptr;

  if (!ok) {
    return false;
  }

  // Downscale further with a proper filter if JPEGDEC's power-of-two scaling still left us
  // bigger than we need -- see boxDownscale() above.
  const uint16_t *finalBuf = pixelBuf;
  int finalW = outW;
  int finalH = outH;
  int decodedShortest = min(outW, outH);
  if (decodedShortest > kWorkingSize) {
    float scale = (float)kWorkingSize / decodedShortest;
    int smallW = max(1, (int)(outW * scale));
    int smallH = max(1, (int)(outH * scale));
    size_t smallBytes = (size_t)smallW * smallH * 2;
    if (ensureSmallCap(smallBytes)) {
      boxDownscale(pixelBuf, outW, outW, outH, smallBuf, smallW, smallH);
      finalBuf = smallBuf;
      finalW = smallW;
      finalH = smallH;
    }
    // If the allocation failed, finalBuf/W/H just stay at the full decoded size -- worse
    // quality on a very low-memory day, not a missing image.
  }

  desc.header.always_zero = 0;
  desc.header.w = finalW;
  desc.header.h = finalH;
  desc.header.cf = LV_IMG_CF_TRUE_COLOR;
  desc.data_size = (size_t)finalW * finalH * 2;
  desc.data = (const uint8_t *)finalBuf;

  lastUrl = url;
  return true;
}

const lv_img_dsc_t *descriptor() { return desc.data ? &desc : nullptr; }

const String &currentUrl() { return lastUrl; }

bool makeThumbnail(uint16_t *dst, int size) {
  if (!desc.data) {
    return false;
  }
  // Center square crop, so non-square covers fill the tile the same way the art inset does.
  int w = desc.header.w, h = desc.header.h;
  int side = min(w, h);
  const uint16_t *src = (const uint16_t *)desc.data;
  const uint16_t *crop = &src[((h - side) / 2) * w + (w - side) / 2];
  boxDownscale(crop, w, side, side, dst, size, size);
  return true;
}

} // namespace albumart
} // namespace wiim
