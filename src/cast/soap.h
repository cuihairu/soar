// AVTransport SOAP control for the casting control point.
//
// Builds the complete HTTP requests (headers + envelope) for the five
// transport actions — SetAVTransportURI, Play, Pause, Seek, Stop — plus
// GetPositionInfo, the only state read (GENA eventing is deliberately
// not done; the session polls). Also parses the replies' two interesting
// shapes: a UPnP fault and a position report. Pure string work; the HTTP
// side is renderer.cpp.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace soar::cast {

// A renderer-reported position (the fields the control point consumes of
// a GetPositionInfoResponse). Durations keep their wire form until
// parseClockMs turns them into milliseconds; "NOT_IMPLEMENTED" and empty
// strings stay unparsed (-1) rather than pretending to be zero.
struct PositionInfo {
  int track = 0;
  std::string duration;
  std::string rel_time;
};

// A UPnP error from a rejected action (AVTransport spec action failed).
struct SoapFault {
  int code = 0;
  std::string description;
};

// The AVTransport service type every DMR speaks; the SOAPACTION header
// and the envelope's action namespace both carry it.
extern const char kAvTransportType[];

// ---- request builders (full HTTP POST text, body last) ----

// `protocol_info` ("" allowed) becomes the <res> protocolInfo attribute —
// the stream type a picky renderer filters on (the transcode session
// advertises "http-get:*:video/mpeg:*"). Empty keeps the attribute off.
std::string setAvTransportUri(const std::string& control_url, const std::string& uri,
                              const std::string& title,
                              const std::string& protocol_info = "");
std::string play(const std::string& control_url);
// Pause carries no arguments; resume is Play at speed 1 — the AVTransport
// spec has no separate Resume action.
std::string pause(const std::string& control_url);
std::string stop(const std::string& control_url);
// Unit REL_TIME — the position form a playing renderer seeks with.
std::string seekRelTime(const std::string& control_url, std::int64_t ms);
std::string getPositionInfo(const std::string& control_url);

// ---- reply parsing ----

// UPnP error body of a rejected action. False when the body carries no
// <UPnPError> (a plain 500 or an unrelated document is not a fault).
bool parseFault(std::string_view body, SoapFault* out);

bool parsePositionInfo(std::string_view body, PositionInfo* out);

// "H:MM:SS" / "HH:MM:SS" (the wire forms, optional fractional seconds
// ignored) → milliseconds; -1 for anything unparsable including
// "NOT_IMPLEMENTED".
std::int64_t parseClockMs(const std::string& clock);

// Inverse: 90500 → "0:01:30" (the H:MM:SS form renderers report).
std::string formatClockMs(std::int64_t ms);

}  // namespace soar::cast
