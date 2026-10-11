#include "device_xml.h"

#include <cstdlib>

#include "text_util.h"

namespace soar::cast {

namespace {

// Anything the parser can do with must name a device or a service. This is
// the cheap way to tell a device description from an HTML error page or
// plain text served off a wrong port.
bool looksLikeDescription(std::string_view xml) {
  for (size_t i = 0; i < xml.size(); ++i) {
    if (xml[i] != '<') continue;
    // ciEqual wants equal sizes: the literal lengths are 7 and 8.
    if (i + 7 <= xml.size() && ciEqual(xml.substr(i, 7), "<device")) return true;
    if (i + 8 <= xml.size() && ciEqual(xml.substr(i, 8), "<service")) return true;
  }
  return false;
}

// "http://host:port/rest" → (authority without scheme, path with query).
void splitAuthority(std::string_view url, std::string* authority, std::string* rest) {
  *authority = "";
  *rest = "";
  const size_t scheme_end = url.find("://");
  if (scheme_end == std::string::npos || scheme_end == 0) return;
  const size_t end = url.find('/', scheme_end + 3);
  *authority = std::string(url.substr(0, end));
  *rest = end == std::string::npos ? "" : std::string(url.substr(end));
}

}  // namespace

bool parseDeviceDescription(std::string_view xml, DeviceDesc* out) {
  if (out == nullptr) return false;
  *out = DeviceDesc{};
  if (!looksLikeDescription(xml)) return false;

  out->device_type = tagText(xml, "deviceType");
  out->friendly_name = tagText(xml, "friendlyName");
  out->udn = tagText(xml, "UDN");

  // Every <service> block is one entry; <serviceType> never matches the
  // "service" tag scan (the name boundary rules exclude it).
  for (const std::string& block : tagTexts(xml, "service")) {
    ServiceDesc service;
    service.service_type = tagText(block, "serviceType");
    service.service_id = tagText(block, "serviceId");
    service.control_url = tagText(block, "controlURL");
    service.eventsub_url = tagText(block, "eventSubURL");
    if (!service.service_type.empty()) out->services.push_back(std::move(service));
  }
  return true;
}

bool avTransportControlUrl(const DeviceDesc& device, const std::string& base,
                           std::string* out) {
  if (out == nullptr) return false;
  const std::string prefix = "urn:schemas-upnp-org:service:avtransport:";
  for (const ServiceDesc& service : device.services) {
    const std::string type = lower(service.service_type);
    if (type.size() <= prefix.size() ||
        !ciEqual(type.substr(0, prefix.size()), prefix))
      continue;
    const std::string url = resolveUrl(base, service.control_url);
    if (!url.empty()) {
      *out = url;
      return true;
    }
  }
  return false;
}

std::string resolveUrl(const std::string& base, const std::string& ref) {
  if (ref.empty()) return "";
  std::string merged = ref;
  const size_t fragment = merged.find('#');
  if (fragment != std::string::npos) merged = merged.substr(0, fragment);
  // Already absolute: the device gave a full control URL.
  if (merged.find("://") != std::string::npos) return merged;

  std::string authority, base_path;
  splitAuthority(base, &authority, &base_path);
  if (authority.empty()) return "";  // no origin to hang the reference on

  if (!merged.empty() && merged[0] == '/') return authority + merged;
  // A relative reference resolves against the base directory: the base
  // document's own last segment comes off, and its query does not carry
  // over into a resolved path.
  if (!base_path.empty()) {
    const size_t slash = base_path.find_last_of('/');
    const std::string dir = slash == std::string::npos ? "" : base_path.substr(0, slash + 1);
    return authority + dir + merged;
  }
  return authority + merged;
}

bool splitUrl(const std::string& url, UrlParts* out) {
  if (out == nullptr) return false;
  *out = UrlParts{};
  const std::string_view view(url);
  const std::string scheme = "http://";
  if (view.size() < scheme.size() || !ciEqual(view.substr(0, scheme.size()), scheme))
    return false;  // https control points are not in scope
  const std::string_view rest = view.substr(scheme.size());
  const size_t path_at = rest.find_first_of("/?#");
  std::string authority(rest.substr(0, path_at));
  if (authority.empty()) return false;

  std::string port_part;
  if (authority[0] == '[') {  // bracketed IPv6 literal
    const size_t rb = authority.find(']');
    if (rb == std::string::npos) return false;
    out->host = authority.substr(1, rb - 1);
    port_part = authority.substr(rb + 1);
    if (!port_part.empty()) {
      if (port_part[0] != ':') return false;
      port_part.erase(0, 1);
    }
  } else {
    const size_t colon = authority.find(':');
    if (colon == std::string::npos) {
      out->host = authority;
    } else {
      out->host = authority.substr(0, colon);
      port_part = authority.substr(colon + 1);
    }
  }
  if (out->host.empty()) return false;

  if (!port_part.empty()) {
    if (port_part.find_first_not_of("0123456789") != std::string::npos) return false;
    const int port = std::atoi(port_part.c_str());
    if (port <= 0 || port > 65535) return false;
    out->port = port;
  }
  if (path_at != std::string::npos) out->path = rest.substr(path_at);
  return true;
}

}  // namespace soar::cast
