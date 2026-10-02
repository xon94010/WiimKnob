#pragma once

#include "RecentAlbums.h"
#include "WiimClient.h"

#include <lvgl.h>

// Runs all WiiM networking (status/metadata polling, album art fetch+decode, and
// prev/next/pause/volume commands) on its own FreeRTOS task, pinned away from the core that
// runs loop() (display, touch, knob). A cold HTTPS connection to the WiiM costs low seconds on
// this CPU -- doing that inline in loop() froze the whole UI for the duration of every single
// command. Everything here is non-blocking from the caller's side: requests just enqueue work,
// and reads just copy out the latest known state under a mutex.
namespace wiim {
namespace task {

// Starts the background task. Call once from setup().
void begin();

// Non-blocking: enqueues the command for the background task to send as soon as it can.
void requestNext();
void requestPrevious();
void requestTogglePause();
void requestPreset(int key); // 1-based preset slot
void requestRecent(int slot); // recent::Entry::slot -- replays that saved album

// Non-blocking: only the latest value before each send matters, so rapid knob turns collapse
// into a handful of network calls instead of one per detent.
void requestVolume(int volume0to100);

// Non-blocking snapshot reads, safe to call every loop() iteration.
bool isConnected();
PlayerState getPlayerState();
TrackMetadata getMetadata();
const lv_img_dsc_t *getAlbumArt();

// Bumped whenever title/artist/album art changes. Compare against a value you saved from a
// previous call to know whether it's worth re-reading getMetadata()/getAlbumArt().
uint32_t metadataVersion();

// Same pattern for the preset list: refreshed at startup and every few minutes after.
PresetList getPresets();
uint32_t presetsVersion();

// And for recently played albums (most recent first), saved whenever a new album starts.
recent::List getRecent();
const lv_img_dsc_t *getRecentThumbnail(int slot); // nullptr if that album had no art
uint32_t recentVersion();

} // namespace task
} // namespace wiim
