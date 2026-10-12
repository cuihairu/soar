// UPnP device-description parsing for the casting control point.
//
// A renderer found over SSDP answers an HTTP GET on its LOCATION with an
// XML document (the device description) listing embedded services and
// their control URLs — often relative, resolved against the description
// URL itself. This file is the plain-string half: parse the document,
// resolve URLs, split them into connectable parts. The HTTP fetch lives
// in renderer.cpp.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace soar::cast {

// One <service> entry of a device description.
struct ServiceDesc {
  std::string service_type;  // "urn:schemas-upnp-org:service:AVTransport:1"
  std::string service_id;
  std::string control_url;   // verbatim from <controlURL> — often relative
  std::string eventsub_url;  // verbatim from <eventSubURL> — often relative
};

struct DeviceDesc {
  std::string device_type;
  std::string friendly_name;
  std::string udn;
  std::vector<ServiceDesc> services;  // every <service> in <serviceList>
};

// Parses a device description. Tolerates attribute-bearing tags and
// leading whitespace/declarations; a document with no <serviceList>
// parses true with an empty service list (the caller then reports "no
// AVTransport"). Returns false on a document without any tag the parser
// looks for (not a device description at all).
bool parseDeviceDescription(std::string_view xml, DeviceDesc* out);

// First service whose type names AVTransport:1, with its control URL
// resolved against `base` (the description URL). Returns false when the
// device has no AVTransport service or the URL does not resolve.
bool avTransportControlUrl(const DeviceDesc& device, const std::string& base,
                           std::string* out);

// Same match, the service's <eventSubURL> instead. Returns false when the
// service exists but advertises no subscription endpoint (eventing is
// best-effort everywhere: a renderer without one stays poll-only).
bool avTransportEventUrl(const DeviceDesc& device, const std::string& base,
                         std::string* out);

// RFC 3986 §5.3 reference resolution, the subset device descriptions use:
// absolute references pass through, "/path" attaches to scheme://authority,
// relative paths merge into the base directory, queries/fragments in the
// base are dropped when a path is resolved. "" on a base without scheme.
std::string resolveUrl(const std::string& base, const std::string& ref);

// The pieces an HTTP client needs to reach a URL. Default port 80 for
// scheme http (the only scheme the control point speaks). Returns false
// for anything without an http:// scheme or a host.
struct UrlParts {
  std::string host;
  int port = 80;
  std::string path;  // begins with '/' (or "" for a bare origin)
};
bool splitUrl(const std::string& url, UrlParts* out);

}  // namespace soar::cast
