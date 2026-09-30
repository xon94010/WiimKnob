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

  desc.header.always_zero = 0;
  desc.header.w = outW;
  desc.header.h = outH;
  desc.header.cf = LV_IMG_CF_TRUE_COLOR;
  desc.data_size = neededBytes;
  desc.data = (const uint8_t *)pixelBuf;

  lastUrl = url;
  return true;
}

const lv_img_dsc_t *descriptor() { return desc.data ? &desc : nullptr; }

} // namespace albumart
} // namespace wiim
