#include "WiimTask.h"
#include "AlbumArt.h"
#include "Config.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace wiim {
namespace task {

namespace {

enum class Cmd { kNext, kPrevious, kTogglePause };

// Coalesce rapid knob turns into one send every this many ms, rather than one per detent.
constexpr uint32_t kVolumeSendIntervalMs = 120;

WiimClient client(WIIM_HOST);

SemaphoreHandle_t stateMutex;
QueueHandle_t cmdQueue;

PlayerState latestPlayer;
TrackMetadata latestMeta;
bool connected = false;
uint32_t metaVersionCounter = 0;

bool volumeDirty = false;
int pendingVolume = -1;

String lastAlbumArtUrl;

void refreshStatus() {
  PlayerState state;
  bool ok = client.fetchStatus(state);

  xSemaphoreTake(stateMutex, portMAX_DELAY);
  connected = ok;
  if (ok) {
    latestPlayer = state;
  }
  xSemaphoreGive(stateMutex);
}

void refreshMetadata() {
  TrackMetadata meta;
  if (!client.fetchMetadata(meta)) {
    return;
  }

  bool artChanged = false;
  if (meta.albumArtUrl.length() > 0 && meta.albumArtUrl != lastAlbumArtUrl) {
    if (wiim::albumart::fetch(meta.albumArtUrl)) {
      lastAlbumArtUrl = meta.albumArtUrl;
      artChanged = true;
    }
  }

  xSemaphoreTake(stateMutex, portMAX_DELAY);
  bool textChanged = meta.title != latestMeta.title || meta.artist != latestMeta.artist;
  latestMeta = meta;
  if (textChanged || artChanged) {
    metaVersionCounter++;
  }
  xSemaphoreGive(stateMutex);
}

void taskFn(void *) {
  uint32_t lastStatusMs = 0;
  uint32_t lastMetaMs = 0;
  uint32_t lastVolumeSendMs = 0;
  bool forceStatus = true;
  bool forceMeta = true;

  for (;;) {
    // User-initiated commands go out before the next scheduled poll.
    Cmd cmd;
    while (xQueueReceive(cmdQueue, &cmd, 0) == pdTRUE) {
      switch (cmd) {
      case Cmd::kNext:
        client.next();
        break;
      case Cmd::kPrevious:
        client.previous();
        break;
      case Cmd::kTogglePause:
        client.togglePause();
        break;
      }
      forceStatus = true;
    }

    uint32_t now = millis();

    bool sendVolume = false;
    int volumeToSend = 0;
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    if (volumeDirty && now - lastVolumeSendMs >= kVolumeSendIntervalMs) {
      sendVolume = true;
      volumeToSend = pendingVolume;
      volumeDirty = false;
    }
    xSemaphoreGive(stateMutex);
    if (sendVolume) {
      client.setVolume(volumeToSend);
      lastVolumeSendMs = now;
    }

    if (forceStatus || now - lastStatusMs >= POLL_STATUS_MS) {
      refreshStatus();
      lastStatusMs = now;
      forceStatus = false;
    }
    if (forceMeta || now - lastMetaMs >= POLL_METADATA_MS) {
      refreshMetadata();
      lastMetaMs = now;
      forceMeta = false;
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

} // namespace

void begin() {
  stateMutex = xSemaphoreCreateMutex();
  cmdQueue = xQueueCreate(8, sizeof(Cmd));
  // Pinned to core 0, away from loop() (core 1 by default under Arduino) -- see WiimTask.h.
  xTaskCreatePinnedToCore(taskFn, "wiim", 16384, nullptr, 1, nullptr, 0);
}

void requestNext() {
  Cmd cmd = Cmd::kNext;
  xQueueSend(cmdQueue, &cmd, 0);
}

void requestPrevious() {
  Cmd cmd = Cmd::kPrevious;
  xQueueSend(cmdQueue, &cmd, 0);
}

void requestTogglePause() {
  Cmd cmd = Cmd::kTogglePause;
  xQueueSend(cmdQueue, &cmd, 0);
}

void requestVolume(int volume0to100) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  pendingVolume = volume0to100;
  volumeDirty = true;
  xSemaphoreGive(stateMutex);
}

bool isConnected() {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  bool c = connected;
  xSemaphoreGive(stateMutex);
  return c;
}

PlayerState getPlayerState() {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  PlayerState copy = latestPlayer;
  xSemaphoreGive(stateMutex);
  return copy;
}

TrackMetadata getMetadata() {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  TrackMetadata copy = latestMeta;
  xSemaphoreGive(stateMutex);
  return copy;
}

const lv_img_dsc_t *getAlbumArt() { return wiim::albumart::descriptor(); }

uint32_t metadataVersion() {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  uint32_t v = metaVersionCounter;
  xSemaphoreGive(stateMutex);
  return v;
}

} // namespace task
} // namespace wiim
