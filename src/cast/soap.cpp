#include "soap.h"

#include <cstdio>
#include <cstdlib>

#include "device_xml.h"
#include "text_util.h"

namespace soar::cast {

const char kAvTransportType[] = "urn:schemas-upnp-org:service:AVTransport:1";

namespace {

// One SOAP-over-HTTP POST: the envelope is
//   <s:Envelope ...><s:Body><u:Action xmlns:u=TYPE>args</u:Action></s:Body>
// and the headers name the same service type in SOAPACTION. Content-Length
// is computed from the body actually built, so golden-text tests can pin
// header and body against each other.
std::string envelope(const std::string& control_url, const std::string& action,
                     const std::string& args) {
  const std::string body =
      std::string("<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n") +
      "<s:Envelope "
      "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
      "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
      "<s:Body><u:" + action + " xmlns:u=\"" + kAvTransportType + "\">" + args +
      "</u:" + action + "></s:Body></s:Envelope>";

  UrlParts parts;
  // A control URL that does not split still builds a request (the HTTP
  // layer rejects it); the HOST header just carries what there is.
  std::string host = control_url;
  if (splitUrl(control_url, &parts)) {
    host = parts.host + ":" + std::to_string(parts.port);
  }

  std::string out;
  out.reserve(160 + body.size());
  out += "POST " + (parts.path.empty() ? std::string("/") : parts.path) + " HTTP/1.1\r\n";
  out += "HOST: " + host + "\r\n";
  out += "CONTENT-LENGTH: " + std::to_string(body.size()) + "\r\n";
  out += "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n";
  out += "SOAPACTION: \"" + std::string(kAvTransportType) + "#" + action + "\"\r\n";
  out += "Connection: close\r\n\r\n";
  out += body;
  return out;
}

// <InstanceID>0</InstanceID> starts every AVTransport action argument list
// (first renderers expose exactly one instance).
std::string instanceArg() { return "<InstanceID>0</InstanceID>"; }

}  // namespace

std::string setAvTransportUri(const std::string& control_url, const std::string& uri,
                              const std::string& title) {
  // The metadata a DMR needs to show a title is one minimal video item;
  // the URI is repeated in <res> because some picky renderers read it
  // from there rather than from CurrentURI. Everything user-visible is
  // XML-escaped; the wire form is the escaped text.
  const std::string didl =
      "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" "
      "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
      "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\">"
      "<item id=\"0\" parentID=\"0\" restricted=\"1\">"
      "<dc:title>" + xmlEscape(title) + "</dc:title>"
      "<upnp:class>object.item.videoItem</upnp:class>"
      "<res>" + xmlEscape(uri) + "</res>"
      "</item></DIDL-Lite>";
  return envelope(control_url, "SetAVTransportURI",
                  instanceArg() + "<CurrentURI>" + xmlEscape(uri) + "</CurrentURI>" +
                      "<CurrentURIMetaData>" + xmlEscape(didl) + "</CurrentURIMetaData>");
}

std::string play(const std::string& control_url) {
  return envelope(control_url, "Play", instanceArg() + "<Speed>1</Speed>");
}

std::string stop(const std::string& control_url) {
  return envelope(control_url, "Stop", instanceArg());
}

std::string seekRelTime(const std::string& control_url, std::int64_t ms) {
  return envelope(control_url, "Seek",
                  instanceArg() + "<Unit>REL_TIME</Unit><Target>" +
                      formatClockMs(ms) + "</Target>");
}

std::string getPositionInfo(const std::string& control_url) {
  return envelope(control_url, "GetPositionInfo", instanceArg());
}

bool parseFault(std::string_view body, SoapFault* out) {
  if (out == nullptr) return false;
  *out = SoapFault{};
  const std::string code = tagText(body, "errorCode");
  if (code.empty()) return false;
  out->code = std::atoi(code.c_str());
  out->description = tagText(body, "errorDescription");
  return true;
}

bool parsePositionInfo(std::string_view body, PositionInfo* out) {
  if (out == nullptr) return false;
  *out = PositionInfo{};
  const std::string track = tagText(body, "Track");
  if (track.empty()) return false;  // not a GetPositionInfoResponse
  out->track = std::atoi(track.c_str());
  out->duration = tagText(body, "TrackDuration");
  out->rel_time = tagText(body, "RelTime");
  return true;
}

std::int64_t parseClockMs(const std::string& clock) {
  // H:MM:SS[.frac] — two colons required, every field numeric. The
  // fraction (a legal renderer answer form) is kept as sub-second
  // precision, padded to milliseconds ("5" → 500). Empty and
  // "NOT_IMPLEMENTED" (also legal) parse as -1, "unknown", never as
  // zero: the session's stall detector must not mistake an unimplemented
  // clock for a stuck one.
  size_t c1 = clock.find(':');
  if (c1 == std::string::npos) return -1;
  size_t c2 = clock.find(':', c1 + 1);
  if (c2 == std::string::npos) return -1;
  const std::string h = clock.substr(0, c1);
  const std::string m = clock.substr(c1 + 1, c2 - c1 - 1);
  std::string s = clock.substr(c2 + 1);
  int frac_ms = 0;
  const size_t frac = s.find('.');
  if (frac != std::string::npos) {
    std::string f = s.substr(frac + 1);
    s = s.substr(0, frac);
    if (f.empty()) return -1;
    if (f.find_first_not_of("0123456789") != std::string::npos) return -1;
    // Pad to milliseconds ("5" → "500"), truncate beyond ("1234" → "123").
    if (f.size() > 3) f.resize(3);
    while (f.size() < 3) f.push_back('0');
    frac_ms = std::atoi(f.c_str());
  }
  const auto numeric = [](const std::string& field) {
    return !field.empty() &&
           field.find_first_not_of("0123456789") == std::string::npos;
  };
  if (!numeric(h) || !numeric(m) || !numeric(s)) return -1;
  const long long seconds =
      std::atoll(h.c_str()) * 3600 + std::atoll(m.c_str()) * 60 + std::atoll(s.c_str());
  return seconds * 1000 + frac_ms;
}

std::string formatClockMs(std::int64_t ms) {
  if (ms < 0) ms = 0;
  const std::int64_t total = ms / 1000;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%d:%02d:%02d",
                static_cast<int>(total / 3600),
                static_cast<int>((total / 60) % 60),
                static_cast<int>(total % 60));
  return buf;
}

}  // namespace soar::cast
