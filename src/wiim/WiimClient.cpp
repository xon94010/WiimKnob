#include "WiimClient.h"

namespace wiim {

WiimClient::WiimClient(const char *host) : host_(host) {
  client_.setInsecure(); // local device, self-signed cert -- see WiimClient.h
}

bool WiimClient::getJson(const String &command, JsonDocument &doc) {
  String url = "https://" + host_ + "/httpapi.asp?command=" + command;
  if (!http_.begin(client_, url)) {
    return false;
  }
  http_.setReuse(true); // keep the TLS connection open for the next call
  http_.setTimeout(3000);

  int code = http_.GET();
  if (code != HTTP_CODE_OK) {
    http_.end();
    return false;
  }

  DeserializationError err = deserializeJson(doc, http_.getStream());
  http_.end();
  return err == DeserializationError::Ok;
}

void WiimClient::sendCommandFireAndForget(const String &command) {
  String url = "https://" + host_ + "/httpapi.asp?command=" + command;
  if (!http_.begin(client_, url)) {
    return;
  }
  http_.setReuse(true);
  http_.setTimeout(3000);
  http_.GET();
  http_.end();
}

bool WiimClient::fetchStatus(PlayerState &state) {
  JsonDocument doc;
  if (!getJson("getPlayerStatus", doc)) {
    return false;
  }
  state.status = doc["status"].as<String>();
  state.volume = doc["vol"].as<int>();
  state.muted = doc["mute"].as<int>() != 0;
  return true;
}

bool WiimClient::fetchMetadata(TrackMetadata &meta) {
  JsonDocument doc;
  if (!getJson("getMetaInfo", doc)) {
    return false;
  }
  JsonObject metaData = doc["metaData"];
  if (metaData.isNull()) {
    return false;
  }
  meta.title = metaData["title"].as<String>();
  meta.artist = metaData["artist"].as<String>();
  meta.album = metaData["album"].as<String>();
  meta.albumArtUrl = metaData["albumArtURI"].as<String>();
  return true;
}

void WiimClient::togglePause() { sendCommandFireAndForget("setPlayerCmd:onepause"); }
void WiimClient::next() { sendCommandFireAndForget("setPlayerCmd:next"); }
void WiimClient::previous() { sendCommandFireAndForget("setPlayerCmd:prev"); }

void WiimClient::setVolume(int volume0to100) {
  volume0to100 = constrain(volume0to100, 0, 100);
  sendCommandFireAndForget("setPlayerCmd:vol:" + String(volume0to100));
}

} // namespace wiim
