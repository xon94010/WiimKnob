#include "WiimTask.h"
#include "AlbumArt.h"
#include "RecentAlbums.h"
#include "Config.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace wiim {
namespace task {

namespace {

enum class Cmd { kNext, kPrevious, kTogglePause, kPlayPreset, kPlayRecent };

struct Request {
  Cmd cmd;
  int arg; // preset key for kPlayPreset, storage slot for kPlayRecent, unused otherwise
};

// Coalesce rapid knob turns into one send every this many ms, rather than one per detent.
constexpr uint32_t kVolumeSendIntervalMs = 120;

// Presets only change when edited in the WiiM Home app, so a slow refresh is plenty. Retried
// sooner while we've never had a good fetch (e.g. the WiiM was still booting).
constexpr uint32_t kPresetRefreshMs = 5 * 60 * 1000;
constexpr uint32_t kPresetRetryMs = 15 * 1000;

// Two tasks, each with its own connection to the WiiM (WiimClient isn't thread-safe):
//
//  - "wiim-cmd" sends what the user just did (buttons, presets, recent albums, volume) and
//    polls play state. Everything it does is a quick request on a warm keep-alive connection,
//    so a tap goes out within a few ms. The 1.2s status poll is also what keeps that
//    connection warm: the WiiM drops keep-alive connections after ~5-10s idle, and a cold one
//    costs a TLS handshake (400-700ms) before the next command can go out.
//
//  - "wiim-bg" does everything slow: metadata, album art download + decode, saving recently
//    played albums, refreshing the preset list. Any of those can take seconds, and they used
//    to share one task with the commands -- a preset tap could sit in the queue until an album
//    art download finished.
WiimClient cmdClient(WIIM_HOST);
WiimClient bgClient(WIIM_HOST);

SemaphoreHandle_t stateMutex;
QueueHandle_t cmdQueue;

PlayerState latestPlayer;
TrackMetadata latestMeta;
bool connected = false;
uint32_t metaVersionCounter = 0;
PresetList latestPresets;
uint32_t presetsVersionCounter = 0;
bool havePresets = false;
recent::List latestRecent;
uint32_t recentVersionCounter = 0;

bool volumeDirty = false;
int pendingVolume = -1;

// Set by the command task after something that likely changed the track (preset, replay,
// next/prev), so the background task re-reads metadata right away instead of on its timer.
volatile bool metaRefreshRequested = false;

String lastAlbumArtUrl;

// Some sources (seen with Qobuz Connect) report status "none" whether playing or paused. Infer
// it from whether the track position is moving instead. Paused only after two unchanged
// readings in a row, since the WiiM's position can lag a poll behind while playing.
void inferPlayState(PlayerState &state) {
  static long lastPosition = -1;
  static int unchangedCount = 0;
  if (state.status != "none" && state.status.length() > 0) {
    lastPosition = -1;
    return;
  }
  if (lastPosition >= 0 && state.positionMs == lastPosition) {
    unchangedCount++;
  } else if (lastPosition >= 0) {
    unchangedCount = 0;
  }
  state.status = (lastPosition >= 0 && unchangedCount < 2) ? "play" : "pause";
  lastPosition = state.positionMs;
}

void refreshStatus() {
  PlayerState state;
  bool ok = cmdClient.fetchStatus(state);
  if (ok) {
    inferPlayState(state);
  }

  xSemaphoreTake(stateMutex, portMAX_DELAY);
  connected = ok;
  if (ok) {
    latestPlayer = state;
  }
  xSemaphoreGive(stateMutex);
}

void publishRecent() {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  latestRecent = recent::list();
  recentVersionCounter++;
  xSemaphoreGive(stateMutex);
}

void refreshMetadata() {
  TrackMetadata meta;
  if (!bgClient.fetchMetadata(meta)) {
    return;
  }

  bool artChanged = false;
  if (meta.albumArtUrl.startsWith("http") && meta.albumArtUrl != lastAlbumArtUrl) {
    // Free this task's own TLS session first: with the command task's one also open, there
    // isn't internal RAM for a third (see WiimClient::closeConnection). It reconnects on the
    // next metadata poll, which only costs this background task a handshake.
    bgClient.closeConnection();
    if (wiim::albumart::fetch(meta.albumArtUrl)) {
      lastAlbumArtUrl = meta.albumArtUrl;
      artChanged = true;
    } else {
      Serial.printf("[art] fetch failed (internal heap free %u, largest block %u)\n",
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }
  }

  xSemaphoreTake(stateMutex, portMAX_DELAY);
  bool textChanged = meta.title != latestMeta.title || meta.artist != latestMeta.artist ||
                     meta.album != latestMeta.album;
  latestMeta = meta;
  if (textChanged || artChanged) {
    metaVersionCounter++;
  }
  PlayerState player = latestPlayer;
  xSemaphoreGive(stateMutex);

  // After the art fetch above, so a newly saved album gets this album's thumbnail.
  if (recent::notePlaying(bgClient, meta, player)) {
    publishRecent();
  }
}

void refreshPresets() {
  PresetList list;
  if (!bgClient.fetchPresets(list)) {
    Serial.println("[presets] fetch failed");
    return;
  }
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  latestPresets = list;
  havePresets = true;
  presetsVersionCounter++;
  xSemaphoreGive(stateMutex);
  Serial.printf("[presets] %d found\n", list.count);
}

void runCommand(const Request &req) {
  uint32_t started = millis();
  bool ok = true;
  switch (req.cmd) {
  case Cmd::kNext:
    ok = cmdClient.next();
    break;
  case Cmd::kPrevious:
    ok = cmdClient.previous();
    break;
  case Cmd::kTogglePause:
    ok = cmdClient.togglePause();
    break;
  case Cmd::kPlayPreset:
    ok = cmdClient.playPreset(req.arg);
    Serial.printf("[cmd] preset %d %s in %lums\n", req.arg, ok ? "sent" : "FAILED", millis() - started);
    break;
  case Cmd::kPlayRecent:
    ok = recent::replay(cmdClient, req.arg);
    Serial.printf("[cmd] replay slot %d %s in %lums\n", req.arg, ok ? "sent" : "FAILED", millis() - started);
    break;
  }
  if (!ok) {
    Serial.printf("[cmd] command %d failed\n", (int)req.cmd);
  }
  if (req.cmd != Cmd::kTogglePause) {
    metaRefreshRequested = true;
  }
}

void cmdTaskFn(void *) {
  uint32_t lastStatusMs = 0;
  uint32_t lastVolumeSendMs = 0;
  bool forceStatus = true;

  for (;;) {
    // Blocks until a command arrives or it's time for something else, so a tap is picked up
    // immediately rather than on the next loop tick.
    Request req;
    if (xQueueReceive(cmdQueue, &req, pdMS_TO_TICKS(20)) == pdTRUE) {
      do {
        runCommand(req);
      } while (xQueueReceive(cmdQueue, &req, 0) == pdTRUE);
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
      cmdClient.setVolume(volumeToSend);
      lastVolumeSendMs = now;
    }

    if (forceStatus || now - lastStatusMs >= POLL_STATUS_MS) {
      refreshStatus();
      lastStatusMs = now;
      forceStatus = false;
    }
  }
}

void bgTaskFn(void *) {
  recent::begin();
  publishRecent();

  uint32_t lastMetaMs = 0;
  uint32_t lastPresetsMs = 0;
  bool forcePresets = true;
  bool forceMeta = true;

  for (;;) {
    uint32_t now = millis();
    if (metaRefreshRequested) {
      metaRefreshRequested = false;
      forceMeta = true;
    }
    if (forceMeta || now - lastMetaMs >= POLL_METADATA_MS) {
      refreshMetadata();
      lastMetaMs = now;
      forceMeta = false;
    }
    uint32_t presetInterval = havePresets ? kPresetRefreshMs : kPresetRetryMs;
    if (forcePresets || now - lastPresetsMs >= presetInterval) {
      refreshPresets();
      lastPresetsMs = now;
      forcePresets = false;
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

} // namespace

void begin() {
  stateMutex = xSemaphoreCreateMutex();
  cmdQueue = xQueueCreate(8, sizeof(Request));
  // Pinned to core 0, away from loop() (core 1 by default under Arduino) -- see WiimTask.h.
  // The command task gets the higher priority so it preempts background work as soon as a
  // tap comes in (both mostly sit blocked on the network, so neither starves the other).
  xTaskCreatePinnedToCore(cmdTaskFn, "wiim-cmd", 12288, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(bgTaskFn, "wiim-bg", 16384, nullptr, 1, nullptr, 0);
}

void requestNext() {
  Request req = {Cmd::kNext, 0};
  xQueueSend(cmdQueue, &req, 0);
}

void requestPrevious() {
  Request req = {Cmd::kPrevious, 0};
  xQueueSend(cmdQueue, &req, 0);
}

void requestTogglePause() {
  Request req = {Cmd::kTogglePause, 0};
  xQueueSend(cmdQueue, &req, 0);
}

void requestPreset(int key) {
  Request req = {Cmd::kPlayPreset, key};
  xQueueSend(cmdQueue, &req, 0);
}

void requestRecent(int slot) {
  Request req = {Cmd::kPlayRecent, slot};
  xQueueSend(cmdQueue, &req, 0);
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

PresetList getPresets() {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  PresetList copy = latestPresets;
  xSemaphoreGive(stateMutex);
  return copy;
}

recent::List getRecent() {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  recent::List copy = latestRecent;
  xSemaphoreGive(stateMutex);
  return copy;
}

const lv_img_dsc_t *getRecentThumbnail(int slot) { return recent::thumbnail(slot); }

uint32_t recentVersion() {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  uint32_t v = recentVersionCounter;
  xSemaphoreGive(stateMutex);
  return v;
}

uint32_t presetsVersion() {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  uint32_t v = presetsVersionCounter;
  xSemaphoreGive(stateMutex);
  return v;
}

} // namespace task
} // namespace wiim
