#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

namespace wiim {

struct PlayerState {
  String status;   // "play", "pause", "stop", "buffering"
  int volume = 0;   // 0-100
  bool muted = false;
  int queueCount = 0; // tracks in the current play queue (plicount); 0 for e.g. Spotify Connect
  int mode = 0;       // playback source: 10 = WiiM app queue, 31 Spotify Connect, 36 Qobuz
                      // Connect, 1 AirPlay, 40+ physical inputs (line-in, optical, ...)
  long positionMs = 0;
};

struct TrackMetadata {
  String title;
  String artist;
  String album;
  String albumArtUrl;
};

constexpr int kMaxPresets = 12; // WiiM Home allows 12 preset slots

// One filled preset slot. Presets on this firmware generation are "routines" -- a list of
// steps (switch input, switch output, load an EQ, ...) stored on the device itself -- so we can
// describe what each one does but there's no icon or artwork to show for it.
struct Preset {
  int key = 0;            // 1-based slot number, what MCUKeyShortClick takes
  String name;
  String input;           // the routine's audioInput step ("line-in", "wifi", ...), empty if none
  bool usbOutput = false; // routine switches output to a USB DAC
  bool changesEq = false;
};

struct PresetList {
  Preset items[kMaxPresets];
  int count = 0;
  // The "wifi" input really means "network streaming"; whether that's over Wi-Fi or Ethernet
  // depends on how the WiiM itself is connected, which getStatusEx tells us.
  bool wiimOnEthernet = false;
};

// Talks to a WiiM device's local HTTP API (https://{ip}/httpapi.asp?command=...).
// Reference: https://cvdlinden.github.io/wiim-httpapi/ and
// https://www.wiimhome.com/pdf/HTTP%20API%20for%20WiiM%20Products.pdf
class WiimClient {
public:
  explicit WiimClient(const char *host);

  // GET getPlayerStatus. Returns false on network/parse failure (state left unchanged).
  bool fetchStatus(PlayerState &state);

  // GET getMetaInfo. Returns false on network/parse failure (state left unchanged).
  bool fetchMetadata(TrackMetadata &meta);

  // Reads which routine sits in each preset slot (UPnP GetKeyMapping -- the HTTP API's own
  // getPresetInfo reports 0 presets for routine-based ones) and joins that with the routine
  // definitions from getAllRoutines. Returns false on network/parse failure (list unchanged).
  bool fetchPresets(PresetList &list);

  // Commands return false only if the WiiM never got them (after one retry).
  bool togglePause();
  bool next();
  bool previous();
  bool setVolume(int volume0to100);
  bool playPreset(int key); // 1-based slot, same as pressing it in the WiiM Home app

  // UPnP PlayQueue service call (plain HTTP, port 49152). argsXml goes inside the action
  // element as-is, so it must already be XML-escaped. On success, *response (if given) gets
  // the raw SOAP response body.
  bool soap(const char *action, const String &argsXml, String *response = nullptr,
            size_t maxResponseBytes = 16 * 1024);

  // Drops the kept-alive TLS connection. Each open TLS session pins ~40KB of internal RAM
  // (this framework build can't put mbedTLS buffers in PSRAM), and there's only room for two
  // at once -- callers about to open a third (album art) close this one first.
  void closeConnection();

private:
  bool getJson(const String &command, JsonDocument &doc, const JsonDocument *filter = nullptr);
  bool fetchKeyMapping(String &decodedXml);
  bool sendCommand(const String &command);

  String host_;

  // Kept open and reused across requests. A fresh TLS handshake costs several hundred ms on
  // this CPU (confirmed against a real WiiM: ~9ms per request once connected, vs. the
  // handshake itself dominating a cold connection) -- creating a new WiFiClientSecure per
  // call, as an earlier version of this file did, paid that cost on every single command.
  //
  // HTTPClient itself must be persistent too, not just the WiFiClientSecure: its destructor
  // unconditionally calls _client->stop(), so a local `HTTPClient http;` per call closes the
  // very connection its own end()-with-reuse just decided to keep open. Making both members
  // is what actually gets the reuse (confirmed on-device: cold calls were consistently
  // 400-700ms with the connection never actually surviving between requests before this).
  //
  // HTTPClient detects a dropped connection and reconnects on its own, so this is safe to
  // reuse even if the server closes it after some idle time.
  WiFiClientSecure client_;
  HTTPClient http_;
};

// Small XML helpers for the UPnP responses (not a real parser -- these are flat, predictable).
String tagValue(const String &xml, const char *tag); // first <tag>...</tag>, or empty
void xmlUnescapeOnce(String &s);                     // one level of entity decoding

} // namespace wiim
