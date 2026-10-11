// SSDP discovery messages for the casting control point (docs/mvp.md §3,
// protocol decided 2026-10-11: DLNA/UPnP AV first).
//
// UPnP Device Architecture 1.x, the M-SEARCH/request-response pair only:
// soar's control point searches for renderers and consumes their unicast
// replies. It never listens for NOTIFY, and nothing here touches a socket
// — the wire side lives in renderer.cpp, so this file is plain string
// work that the tests pin against exact bytes.
#pragma once

#include <string>
#include <string_view>

namespace soar::cast {

// "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"
// \r\nMX: <mx>\r\nST: <target>\r\n\r\n" — MX clamped to [1,5], the window
// a renderer gets to answer, per the UPnP UA spec. `search_target` is
// passed through verbatim (the caller owns its value).
std::string makeSearchRequest(const std::string& search_target, int mx);

// One parsed M-SEARCH reply. `location` is the device description URL,
// `usn` the unique service name (the dedup key between repeated replies),
// `st` the search target the device echoed back.
struct SearchResponse {
  std::string location;
  std::string st;
  std::string usn;
};

// Parses one response packet. Header names match case-insensitively; CRLF
// and bare LF both work (renderers disagree). LOCATION, ST and USN are all
// required, so a NOTIFY packet (NT, never ST) parses as false instead of
// sneaking into a discovery result. Returns false for anything else that
// is not a usable reply.
bool parseSearchResponse(std::string_view raw, SearchResponse* out);

}  // namespace soar::cast
