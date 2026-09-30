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
};

struct TrackMetadata {
  String title;
  String artist;
  String album;
  String albumArtUrl;
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

  void togglePause();
  void next();
  void previous();
  void setVolume(int volume0to100);

private:
  bool getJson(const String &command, JsonDocument &doc);
  void sendCommandFireAndForget(const String &command);

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

} // namespace wiim
