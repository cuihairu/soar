#include "eventing.h"

#include <cstdlib>

#include "device_xml.h"
#include "text_util.h"

namespace soar::cast {

namespace {

// The request head the family shares: the event URL splits into the
// request path and the HOST header like every other cast request.
std::string eventHead(const std::string& event_url, const char* method) {
  std::string host = "localhost";
  std::string path = "/";
  UrlParts parts;
  if (splitUrl(event_url, &parts)) {
    host = parts.host + ":" + std::to_string(parts.port);
    if (!parts.path.empty()) path = parts.path;
  }
  std::string out;
  out.reserve(192);
  out += std::string(method) + " " + path + " HTTP/1.1\r\n";
  out += "HOST: " + host + "\r\n";
  return out;
}

// The TIMEOUT field's Second- number; "second-infinite" (any casing) is
// the spec's no-expiry form and reads as 0.
int parseTimeout(const std::string& field) {
  if (ciEqual(field, "second-infinite") || ciEqual(field, "infinite")) return 0;
  const size_t dash = field.rfind('-');
  if (dash == std::string::npos) return 0;
  return std::atoi(field.c_str() + dash + 1);
}

}  // namespace

std::string subscribeRequest(const std::string& event_url,
                             const std::string& callback_url, int timeout_secs) {
  if (timeout_secs < 300) timeout_secs = 300;
  if (timeout_secs > 86400) timeout_secs = 86400;
  std::string out = eventHead(event_url, "SUBSCRIBE");
  out += "CALLBACK: <" + callback_url + ">\r\n";
  out += "NT: upnp:event\r\n";
  out += "TIMEOUT: Second-" + std::to_string(timeout_secs) + "\r\n";
  out += "CONTENT-LENGTH: 0\r\n";
  out += "Connection: close\r\n\r\n";
  return out;
}

std::string renewRequest(const std::string& event_url, const std::string& sid,
                         int timeout_secs) {
  if (timeout_secs < 300) timeout_secs = 300;
  if (timeout_secs > 86400) timeout_secs = 86400;
  std::string out = eventHead(event_url, "SUBSCRIBE");
  out += "SID: " + sid + "\r\n";
  out += "TIMEOUT: Second-" + std::to_string(timeout_secs) + "\r\n";
  out += "CONTENT-LENGTH: 0\r\n";
  out += "Connection: close\r\n\r\n";
  return out;
}

std::string unsubscribeRequest(const std::string& event_url, const std::string& sid) {
  std::string out = eventHead(event_url, "UNSUBSCRIBE");
  out += "SID: " + sid + "\r\n";
  out += "CONTENT-LENGTH: 0\r\n";
  out += "Connection: close\r\n\r\n";
  return out;
}

bool parseSubscribeReply(std::string_view response, Subscription* out) {
  if (out == nullptr) return false;
  *out = Subscription{};
  if (httpStatus(response) != 200) return false;
  out->sid = fieldValue(response, "SID");
  if (out->sid.empty()) return false;
  out->timeout_secs = parseTimeout(fieldValue(response, "TIMEOUT"));
  return true;
}

TransportState parseNotifyState(std::string_view notify_body) {
  const std::string change = tagAttr(notify_body, "LastChange", "val");
  if (change.empty()) return TransportState::Unknown;
  const std::string state = tagAttr(change, "TransportState", "val");
  if (ciEqual(state, "PLAYING")) return TransportState::Playing;
  if (ciEqual(state, "PAUSED_PLAYBACK")) return TransportState::PausedPlayback;
  if (ciEqual(state, "STOPPED")) return TransportState::Stopped;
  if (ciEqual(state, "TRANSITIONING")) return TransportState::Transitioning;
  if (ciEqual(state, "NO_MEDIA_PRESENT")) return TransportState::NoMediaPresent;
  return TransportState::Unknown;
}

}  // namespace soar::cast
