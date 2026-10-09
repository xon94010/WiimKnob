#pragma once

#include "WiimClient.h"

#include <Arduino.h>
#include <lvgl.h>

// The WiiM Home app's "Recently played" list lives in WiiM's cloud, not on the device, so the
// knob keeps its own: whenever a new album starts playing from a queue (Plex, or anything else
// that loads a track list onto the WiiM), it saves a copy of that queue plus a small thumbnail
// to flash. Replaying hands the saved queue back to the WiiM (UPnP CreateQueue) and starts it.
//
// Sources that stream without a device-side queue (Spotify Connect, AirPlay, line-in, ...) have
// nothing to save, so they don't show up here.
//
// The saved queues contain the stream URLs exactly as the WiiM had them -- for Plex that
// includes the server's access token. They only ever live in this device's flash; nothing in
// here logs them.
//
// Everything except snapshot reads must be called from the WiiM task.
namespace wiim {
namespace recent {

constexpr int kMaxAlbums = 10;
constexpr int kThumbSize = 68;

struct Entry {
  int slot = -1; // stable storage slot (0..kMaxAlbums-1); thumbnails and files are keyed on it
  String album;
  String artist; // the album's main artist, worked out from its tracks (not one track's artist)
  String queueName; // XML-escaped, ready to drop into a SOAP argument
};

struct List {
  Entry items[kMaxAlbums]; // most recent first
  int count = 0;
};

// Whether two (album, artist) pairs are the same album. Artists are compared on their main
// artist only, since the WiiM reports each track's artist ("Daft Punk, Pharrell Williams").
bool sameAlbum(const String &albumA, const String &artistA, const String &albumB, const String &artistB);
String primaryArtist(const String &artist);

// Mounts flash storage and loads the saved list + thumbnails.
void begin();

const List &list();

// Thumbnail for a slot, or nullptr if there isn't one. The pointer stays valid forever (the
// pixels behind it get overwritten in place when the slot is reused).
const lv_img_dsc_t *thumbnail(int slot);

// Call after each metadata refresh. If a new album is playing from a queue, saves it and moves
// it to the front. Returns true if the list changed.
bool notePlaying(WiimClient &client, const TrackMetadata &meta, const PlayerState &state);

// Loads the saved queue for `slot` onto the WiiM and starts it from the first track.
bool replay(WiimClient &client, int slot);

} // namespace recent
} // namespace wiim
