#include "WiimClient.h"

namespace wiim {

WiimClient::WiimClient(const char *host) : host_(host) {
  client_.setInsecure(); // local device, self-signed cert -- see WiimClient.h
}

// The WiiM's web server (lighttpd) closes keep-alive connections after ~5-10s idle (measured).
// If that happens just as we reuse one, the request fails at the transport level (negative
// code, no HTTP status at all) -- the WiiM never saw it. Retry those once on a fresh
// connection. Any real HTTP status means the WiiM got the request, so never retry that (a
// repeated next/prev would skip twice).
bool WiimClient::getJson(const String &command, JsonDocument &doc, const JsonDocument *filter) {
  String url = "https://" + host_ + "/httpapi.asp?command=" + command;
  for (int attempt = 0; attempt < 2; attempt++) {
    if (!http_.begin(client_, url)) {
      return false;
    }
    http_.setReuse(true); // keep the TLS connection open for the next call
    http_.setTimeout(3000);

    int code = http_.GET();
    if (code < 0) {
      http_.end();
      client_.stop();
      continue;
    }
    if (code != HTTP_CODE_OK) {
      http_.end();
      return false;
    }

    DeserializationError err = filter ? deserializeJson(doc, http_.getStream(), DeserializationOption::Filter(*filter))
                                      : deserializeJson(doc, http_.getStream());
    http_.end();
    return err == DeserializationError::Ok;
  }
  return false;
}

void WiimClient::closeConnection() {
  http_.end();
  client_.stop();
}

bool WiimClient::sendCommand(const String &command) {
  String url = "https://" + host_ + "/httpapi.asp?command=" + command;
  for (int attempt = 0; attempt < 2; attempt++) {
    if (!http_.begin(client_, url)) {
      return false;
    }
    http_.setReuse(true);
    http_.setTimeout(3000);
    int code = http_.GET();
    if (code > 0) {
      http_.getString(); // drain the body so the connection is clean for the next request
      http_.end();
      return true;
    }
    http_.end();
    client_.stop(); // see the comment above getJson()
  }
  return false;
}

bool WiimClient::fetchStatus(PlayerState &state) {
  JsonDocument doc;
  if (!getJson("getPlayerStatus", doc)) {
    return false;
  }
  state.status = doc["status"].as<String>();
  state.volume = doc["vol"].as<int>();
  state.muted = doc["mute"].as<int>() != 0;
  state.queueCount = doc["plicount"].as<int>();
  state.mode = doc["mode"].as<int>();
  state.positionMs = doc["curpos"].as<long>();
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

// Contents of the first <tag>...</tag> in xml, or empty if it isn't there.
String tagValue(const String &xml, const char *tag) {
  String open = String("<") + tag + ">";
  String close = String("</") + tag + ">";
  int start = xml.indexOf(open);
  if (start < 0) return String();
  start += open.length();
  int end = xml.indexOf(close, start);
  if (end < 0) return String();
  return xml.substring(start, end);
}

void xmlUnescapeOnce(String &s) {
  // &amp; last, so "&amp;lt;" correctly becomes "&lt;" rather than "<".
  s.replace("&lt;", "<");
  s.replace("&gt;", ">");
  s.replace("&quot;", "\"");
  s.replace("&apos;", "'");
  s.replace("&amp;", "&");
}

bool WiimClient::soap(const char *action, const String &argsXml, String *response, size_t maxResponseBytes) {
  // Called rarely (presets refresh, album capture/replay), so a throwaway plain-HTTP connection
  // is fine here -- no need for the persistent TLS one getJson() keeps.
  String body;
  body.reserve(argsXml.length() + 400);
  body += "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
          "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:";
  body += action;
  body += " xmlns:u=\"urn:schemas-wiimu-com:service:PlayQueue:1\">";
  body += argsXml;
  body += "</u:";
  body += action;
  body += "></s:Body></s:Envelope>";

  NetworkClient plain;
  HTTPClient http;
  if (!http.begin(plain, "http://" + host_ + ":49152/upnp/control/PlayQueue1")) {
    return false;
  }
  http.setTimeout(5000);
  http.addHeader("Content-Type", "text/xml; charset=\"utf-8\"");
  http.addHeader("SOAPACTION", String("\"urn:schemas-wiimu-com:service:PlayQueue:1#") + action + "\"");
  int code = http.POST((uint8_t *)body.c_str(), body.length());
  if (code != HTTP_CODE_OK) {
    http.end();
    return false;
  }
  if (response) {
    int size = http.getSize(); // -1 if the server didn't say
    if (size > (int)maxResponseBytes) {
      http.end();
      return false;
    }
    *response = http.getString();
    if (response->length() > maxResponseBytes) {
      *response = String();
      http.end();
      return false;
    }
  }
  http.end();
  return true;
}

// The key mapping only lives on the UPnP side, and comes back as an XML document escaped inside
// the SOAP response's <QueueContext>.
bool WiimClient::fetchKeyMapping(String &decodedXml) {
  if (!soap("GetKeyMapping", "", &decodedXml)) {
    return false;
  }
  xmlUnescapeOnce(decodedXml);
  return decodedXml.indexOf("<KeyList>") >= 0;
}

bool WiimClient::fetchPresets(PresetList &list) {
  String keyXml;
  if (!fetchKeyMapping(keyXml)) {
    return false;
  }

  JsonDocument routines;
  if (!getJson("getAllRoutines", routines)) {
    return false;
  }

  PresetList result;
  for (int key = 1; key <= kMaxPresets && result.count < kMaxPresets; key++) {
    String block = tagValue(keyXml, ("Key" + String(key)).c_str());
    String routineId = tagValue(block, "RoutineId");
    if (routineId.length() == 0 || routineId == "Empty") {
      // A plain music preset (station/playlist) would carry its own <Name> instead of a
      // routine -- none to test against here, but show it by name if one turns up.
      String name = tagValue(block, "Name");
      if (name.length() == 0) continue;
      Preset &p = result.items[result.count++];
      p.key = key;
      p.name = name;
      continue;
    }

    for (JsonObject routine : routines["routines"].as<JsonArray>()) {
      if (routineId != routine["id"].as<const char *>()) continue;
      Preset &p = result.items[result.count++];
      p.key = key;
      p.name = routine["name"].as<String>();
      for (JsonObject step : routine["steps"].as<JsonArray>()) {
        String type = step["type"].as<String>();
        if (type == "audioInput") {
          p.input = step["payload"]["input"].as<String>();
        } else if (type == "audioOutput") {
          p.usbOutput = String(step["payload"]["output"].as<const char *>()).indexOf("UAC") >= 0;
        } else if (type == "EQ") {
          p.changesEq = true;
        }
      }
      break;
    }
  }

  JsonDocument filter;
  filter["eth0"] = true;
  JsonDocument status;
  if (getJson("getStatusEx", status, &filter)) {
    String eth = status["eth0"].as<String>();
    result.wiimOnEthernet = eth.length() > 0 && eth != "0.0.0.0";
  }

  list = result;
  return true;
}

bool WiimClient::playPreset(int key) { return sendCommand("MCUKeyShortClick:" + String(key)); }

bool WiimClient::togglePause() { return sendCommand("setPlayerCmd:onepause"); }
bool WiimClient::next() { return sendCommand("setPlayerCmd:next"); }
bool WiimClient::previous() { return sendCommand("setPlayerCmd:prev"); }

bool WiimClient::setVolume(int volume0to100) {
  volume0to100 = constrain(volume0to100, 0, 100);
  return sendCommand("setPlayerCmd:vol:" + String(volume0to100));
}

} // namespace wiim
