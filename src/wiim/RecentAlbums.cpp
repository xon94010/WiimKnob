#include "RecentAlbums.h"
#include "AlbumArt.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace wiim {
namespace recent {

namespace {

// A long playlist's queue can run to hundreds of KB; an album is ~1.5KB per track. Anything
// bigger than this isn't saved rather than risking a huge allocation.
constexpr size_t kMaxQueueBytes = 96 * 1024;

const char *kDir = "/recent";
const char *kIndexPath = "/recent/index.json";

bool fsReady = false;
// replay() runs on the command task while notePlaying() runs on the background one -- this
// guards the list and the files behind it. Never held across a network call.
SemaphoreHandle_t lock = nullptr;
List entries;
String lastNotedKey;
// A failed capture (e.g. network hiccup) is retried, but not on every 3s metadata poll.
constexpr uint32_t kRetryMs = 20 * 1000;
String lastFailedKey;
uint32_t lastFailedMs = 0;

uint16_t *thumbPixels[kMaxAlbums] = {};
lv_img_dsc_t thumbDesc[kMaxAlbums] = {};
bool haveThumb[kMaxAlbums] = {};

String queuePath(int slot) { return String(kDir) + "/q" + slot + ".xml"; }
String thumbPath(int slot) { return String(kDir) + "/t" + slot + ".rgb"; }

constexpr size_t kThumbBytes = (size_t)kThumbSize * kThumbSize * 2;

// Sources that stream from somewhere else (a phone app, another device, a physical input)
// instead of from a queue on the WiiM -- nothing to save, so don't probe for one.
bool isExternalSource(int mode) {
  return mode == 1 /* AirPlay */ || mode == 2 /* DLNA push */ || mode == 31 /* Spotify Connect */ ||
         mode == 32 /* TIDAL Connect */ || mode == 36 /* Qobuz Connect */ || mode >= 40 /* inputs */;
}

bool isUnknown(const String &s) { return s.length() == 0 || s.startsWith("unknow") || s.startsWith("un_known"); }

void saveIndex() {
  if (!fsReady) return;
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < entries.count; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["slot"] = entries.items[i].slot;
    o["album"] = entries.items[i].album;
    o["artist"] = entries.items[i].artist;
    o["queue"] = entries.items[i].queueName;
  }
  File f = LittleFS.open(kIndexPath, "w");
  if (!f) return;
  serializeJson(doc, f);
  f.close();
}

void loadThumb(int slot) {
  haveThumb[slot] = false;
  File f = LittleFS.open(thumbPath(slot), "r");
  if (!f) return;
  haveThumb[slot] = f.read((uint8_t *)thumbPixels[slot], kThumbBytes) == kThumbBytes;
  f.close();
}

int findByAlbum(const String &album, const String &artist); // defined below

// The album's own artist, worked out from a saved queue: the most common main artist among the
// queue's tracks from this album. The WiiM only reports the current *track's* artist, which
// varies within an album -- on Random Access Memories most tracks are "Daft Punk feat. ..." but
// one is credited to "Thomas Bangalter" alone -- so matching albums on that saved the same album
// several times. Falls back to `fallback` if the queue doesn't say.
String albumArtistFromQueue(const String &rawContext, const String &album, const String &fallback) {
  String decoded = rawContext;
  xmlUnescapeOnce(decoded);
  xmlUnescapeOnce(decoded);

  constexpr int kMaxArtists = 12;
  String names[kMaxArtists];   // original spelling of the first track seen per main artist
  String keys[kMaxArtists];
  int counts[kMaxArtists] = {};
  int distinct = 0;

  int pos = 0;
  while (true) {
    int albumAt = decoded.indexOf("<upnp:album>", pos);
    if (albumAt < 0) break;
    int albumEnd = decoded.indexOf("</upnp:album>", albumAt);
    if (albumEnd < 0) break;
    String trackAlbum = decoded.substring(albumAt + 12, albumEnd);
    // This track's artist is the last <upnp:artist> before its album tag.
    int artistAt = decoded.lastIndexOf("<upnp:artist>", albumAt);
    pos = albumEnd;
    if (artistAt < 0 || !trackAlbum.equalsIgnoreCase(album)) continue;
    int artistEnd = decoded.indexOf("</upnp:artist>", artistAt);
    if (artistEnd < 0 || artistEnd > albumAt) continue;
    String artist = decoded.substring(artistAt + 13, artistEnd);
    String key = primaryArtist(artist);
    int i = 0;
    while (i < distinct && keys[i] != key) i++;
    if (i == distinct) {
      if (distinct == kMaxArtists) continue;
      keys[i] = key;
      names[i] = artist;
      distinct++;
    }
    counts[i]++;
  }
  int best = -1;
  for (int i = 0; i < distinct; i++) {
    if (best < 0 || counts[i] > counts[best]) best = i;
  }
  if (best < 0) return fallback;
  // Keep only the main-artist part of the spelling we saw ("Daft Punk", not "Daft Punk feat. X").
  String name = names[best];
  int cut = name.length();
  String lower = name;
  lower.toLowerCase();
  static const char *kSeparators[] = {",", ";", " feat", " ft.", " featuring", " & ", " and ", " with ", " x ", " / "};
  for (const char *sep : kSeparators) {
    int at = lower.indexOf(sep);
    if (at > 0 && at < cut) cut = at;
  }
  name = name.substring(0, cut);
  name.trim();
  return name;
}


void loadIndex() {
  entries.count = 0;
  File f = LittleFS.open(kIndexPath, "r");
  if (!f) return;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return;
  bool indexChanged = false; // duplicates dropped or artists re-derived: rewrite the index
  for (JsonObject o : doc.as<JsonArray>()) {
    if (entries.count >= kMaxAlbums) break;
    int slot = o["slot"] | -1;
    if (slot < 0 || slot >= kMaxAlbums || !LittleFS.exists(queuePath(slot))) continue;
    String album = o["album"].as<String>();
    String artist = o["artist"].as<String>();
    // Re-derive the album artist from the saved queue, so lists saved before this matched on
    // the album's own artist (see albumArtistFromQueue) merge their duplicates now.
    {
      File q = LittleFS.open(queuePath(slot), "r");
      if (q) {
        String derived = albumArtistFromQueue(q.readString(), album, artist);
        q.close();
        if (derived != artist) {
          artist = derived;
          indexChanged = true;
        }
      }
    }
    // Lists saved before albums were matched on their main artist only can hold the same
    // album several times (one per featured-artist variant) -- keep the newest, drop the rest.
    if (findByAlbum(album, artist) >= 0) {
      LittleFS.remove(queuePath(slot));
      LittleFS.remove(thumbPath(slot));
      indexChanged = true;
      continue;
    }
    Entry &e = entries.items[entries.count++];
    e.slot = slot;
    e.album = album;
    e.artist = artist;
    e.queueName = o["queue"].as<String>();
    loadThumb(slot);
  }
  if (indexChanged) {
    saveIndex();
  }
}

} // namespace

// The WiiM reports the *track's* artist, which can change within one album ("Daft Punk", then
// "Daft Punk, Pharrell Williams, Nile Rodgers") -- matching on the full string saved the same
// album once per variant. Match on the album title plus the main artist only.
String primaryArtist(const String &artist) {
  String a = artist;
  a.toLowerCase();
  static const char *kSeparators[] = {",", ";", " feat", " ft.", " featuring", " & ", " and ", " with ", " x ", " / "};
  int cut = a.length();
  for (const char *sep : kSeparators) {
    int i = a.indexOf(sep);
    if (i > 0 && i < cut) cut = i;
  }
  a = a.substring(0, cut);
  a.trim();
  return a;
}

bool sameAlbum(const String &albumA, const String &artistA, const String &albumB, const String &artistB) {
  return albumA.equalsIgnoreCase(albumB) && primaryArtist(artistA) == primaryArtist(artistB);
}

namespace {

int findByAlbum(const String &album, const String &artist) {
  for (int i = 0; i < entries.count; i++) {
    if (sameAlbum(entries.items[i].album, entries.items[i].artist, album, artist)) return i;
  }
  return -1;
}

int findBySlot(int slot) {
  for (int i = 0; i < entries.count; i++) {
    if (entries.items[i].slot == slot) return i;
  }
  return -1;
}

// Names of the queues the WiiM is holding (it can hold several), XML-escaped. Ones whose track
// count matches what getPlayerStatus reports go first -- though that count reads 0 for some
// sources even while playing, so it's only a hint; the album check in notePlaying() decides.
int listQueues(WiimClient &client, int trackCount, String names[], int maxNames) {
  String xml;
  if (!client.soap("BrowseQueue", "<QueueName>TotalQueue</QueueName>", &xml)) {
    return 0;
  }
  xmlUnescapeOnce(xml);
  int total = tagValue(xml, "TotalQueue").toInt();
  int n = 0;
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 1; i <= total && n < maxNames; i++) {
      String block = tagValue(xml, ("List" + String(i)).c_str());
      bool countMatches = trackCount > 0 && tagValue(block, "TrackNumber").toInt() == trackCount;
      if (countMatches != (pass == 0)) continue;
      String name = tagValue(block, "Name");
      if (name.length() > 0) names[n++] = name;
    }
  }
  return n;
}

// Whether a raw (one-level-escaped) queue holds a track from this album. Track metadata is
// escaped one level deeper again, so decode a copy twice before searching.
bool queueHasAlbum(const String &rawContext, const String &album) {
  String decoded = rawContext;
  xmlUnescapeOnce(decoded);
  xmlUnescapeOnce(decoded);
  return decoded.indexOf("<upnp:album>" + album + "</upnp:album>") >= 0;
}

} // namespace

void begin() {
  lock = xSemaphoreCreateMutex();
  for (int i = 0; i < kMaxAlbums; i++) {
    thumbPixels[i] = (uint16_t *)heap_caps_malloc(kThumbBytes, MALLOC_CAP_SPIRAM);
    thumbDesc[i].header.always_zero = 0;
    thumbDesc[i].header.w = kThumbSize;
    thumbDesc[i].header.h = kThumbSize;
    thumbDesc[i].header.cf = LV_IMG_CF_TRUE_COLOR;
    thumbDesc[i].data_size = kThumbBytes;
    thumbDesc[i].data = (const uint8_t *)thumbPixels[i];
  }

  // Uses the "spiffs" data partition from default_16MB.csv; formats it on first boot.
  fsReady = LittleFS.begin(true);
  if (!fsReady) {
    Serial.println("[recent] flash storage unavailable, history won't persist");
    return;
  }
  if (!LittleFS.exists(kDir)) {
    LittleFS.mkdir(kDir);
  }
  loadIndex();
  Serial.printf("[recent] %d saved albums\n", entries.count);
}

const List &list() { return entries; }

const lv_img_dsc_t *thumbnail(int slot) {
  if (slot < 0 || slot >= kMaxAlbums || !haveThumb[slot] || !thumbPixels[slot]) return nullptr;
  return &thumbDesc[slot];
}

bool notePlaying(WiimClient &client, const TrackMetadata &meta, const PlayerState &state) {
  if (!fsReady || state.status != "play" || isUnknown(meta.album) || isExternalSource(state.mode)) {
    return false;
  }
  // Album title only: the track artist can change within an album (see albumArtistFromQueue).
  String key = meta.album;
  key.toLowerCase();
  if (key == lastNotedKey) {
    return false;
  }
  if (key == lastFailedKey && millis() - lastFailedMs < kRetryMs) {
    return false;
  }
  lastFailedKey = key;
  lastFailedMs = millis();

  constexpr int kMaxQueuesChecked = 3;
  String names[kMaxQueuesChecked];
  int nameCount = listQueues(client, state.queueCount, names, kMaxQueuesChecked);

  // Find the queue this album is actually playing from. Kept exactly as it came (still
  // XML-escaped one level), since that's the form CreateQueue takes it back in.
  String queueName;
  String response;
  int start = -1, end = -1;
  for (int i = 0; i < nameCount; i++) {
    String r;
    if (!client.soap("BrowseQueue", "<QueueName>" + names[i] + "</QueueName>", &r, kMaxQueueBytes + 2048)) {
      continue;
    }
    int s0 = r.indexOf("<QueueContext>");
    int e0 = r.lastIndexOf("</QueueContext>");
    if (s0 < 0 || e0 <= s0) continue;
    s0 += strlen("<QueueContext>");
    if (!queueHasAlbum(r.substring(s0, e0), meta.album)) continue;
    queueName = names[i];
    response = r;
    start = s0;
    end = e0;
    break;
  }
  if (queueName.length() == 0) {
    Serial.printf("[recent] \"%s\" isn't playing from a saved queue (checked %d)\n", meta.album.c_str(), nameCount);
    return false;
  }

  xSemaphoreTake(lock, portMAX_DELAY);
  String albumArtist = albumArtistFromQueue(response.substring(start, end), meta.album, meta.artist);
  int existing = findByAlbum(meta.album, albumArtist);
  int slot;
  if (existing >= 0) {
    slot = entries.items[existing].slot;
  } else if (entries.count < kMaxAlbums) {
    // First slot number not already in use.
    for (slot = 0; slot < kMaxAlbums && findBySlot(slot) >= 0; slot++) {
    }
  } else {
    slot = entries.items[entries.count - 1].slot; // evict the oldest
    existing = entries.count - 1;
  }

  File f = LittleFS.open(queuePath(slot), "w");
  if (!f) {
    xSemaphoreGive(lock);
    return false;
  }
  size_t len = end - start;
  bool wrote = f.write((const uint8_t *)response.c_str() + start, len) == len;
  f.close();
  response = String(); // free it before the thumbnail work
  if (!wrote) {
    xSemaphoreGive(lock);
    return false;
  }

  haveThumb[slot] = false;
  if (albumart::currentUrl() == meta.albumArtUrl && albumart::makeThumbnail(thumbPixels[slot], kThumbSize)) {
    File t = LittleFS.open(thumbPath(slot), "w");
    if (t) {
      t.write((const uint8_t *)thumbPixels[slot], kThumbBytes);
      t.close();
    }
    haveThumb[slot] = true;
  } else {
    LittleFS.remove(thumbPath(slot));
  }

  // Shift everything above `existing` (or the whole list, for a new entry) down one, then put
  // this album at the front.
  int from = existing >= 0 ? existing : entries.count;
  if (existing < 0) entries.count++;
  for (int i = from; i > 0; i--) {
    entries.items[i] = entries.items[i - 1];
  }
  Entry &e = entries.items[0];
  e.slot = slot;
  e.album = meta.album;
  e.artist = albumArtist;
  e.queueName = queueName;
  saveIndex();
  xSemaphoreGive(lock);
  lastNotedKey = key;
  lastFailedKey = String();

  Serial.printf("[recent] saved \"%s\" (%u bytes)\n", meta.album.c_str(), (unsigned)len);
  return true;
}

bool replay(WiimClient &client, int slot) {
  if (!fsReady) return false;

  xSemaphoreTake(lock, portMAX_DELAY);
  int idx = findBySlot(slot);
  File f = idx >= 0 ? LittleFS.open(queuePath(slot), "r") : File();
  if (!f) {
    xSemaphoreGive(lock);
    return false;
  }
  String queueName = entries.items[idx].queueName;
  String args;
  args.reserve(f.size() + 32);
  args += "<QueueContext>";
  args += f.readString();
  args += "</QueueContext>";
  f.close();
  xSemaphoreGive(lock);

  // CreateQueue, not ReplaceQueue: the WiiM only holds the queue that's currently loaded, and
  // ReplaceQueue only swaps the contents of a queue that already exists under that name --
  // for any other album it did nothing, and PlayQueueWithIndex below then fell back to
  // restarting whatever was already loaded. CreateQueue + PlayQueueWithIndex is what the WiiM
  // Home app itself sends when you play an album.
  if (!client.soap("CreateQueue", args)) {
    Serial.println("[recent] CreateQueue failed");
    return false;
  }
  args = String();
  if (!client.soap("PlayQueueWithIndex", "<QueueName>" + queueName + "</QueueName><Index>1</Index>")) {
    Serial.println("[recent] PlayQueueWithIndex failed");
    return false;
  }
  return true;
}

} // namespace recent
} // namespace wiim
