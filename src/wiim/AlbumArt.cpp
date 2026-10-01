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
constexpr int kWorkingSize = 300;

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
void boxDownscale(const uint16_t *src, int srcW, int srcH, uint16_t *dst, int dstW, int dstH) {
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
        const uint16_t *row = &src[sy * srcW];
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

  int divisor = 1;
  int scaleFlag = 0;
  int longest = max(srcW, srcH);
  if (longest > 1200) {
    divisor = 8;
    scaleFlag = JPEG_SCALE_EIGHTH;
  } else if (longest > 600) {
    divisor = 4;
    scaleFlag = JPEG_SCALE_QUARTER;
  } else if (longest > 300) {
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
  int decodedLongest = max(outW, outH);
  if (decodedLongest > kWorkingSize) {
    float scale = (float)kWorkingSize / decodedLongest;
    int smallW = max(1, (int)(outW * scale));
    int smallH = max(1, (int)(outH * scale));
    size_t smallBytes = (size_t)smallW * smallH * 2;
    if (ensureSmallCap(smallBytes)) {
      boxDownscale(pixelBuf, outW, outH, smallBuf, smallW, smallH);
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

} // namespace albumart
} // namespace wiim
