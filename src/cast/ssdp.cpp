#include "ssdp.h"

#include <algorithm>
#include <cstring>

#include "text_util.h"

namespace soar::cast {

std::string makeSearchRequest(const std::string& search_target, int mx) {
  const int clamped = (std::max)(1, (std::min)(mx, 5));
  std::string out;
  out.reserve(96 + search_target.size());
  out += "M-SEARCH * HTTP/1.1\r\n";
  out += "HOST: 239.255.255.250:1900\r\n";
  out += "MAN: \"ssdp:discover\"\r\n";
  out += "MX: " + std::to_string(clamped) + "\r\n";
  out += "ST: " + search_target + "\r\n";
  out += "\r\n";
  return out;
}

bool parseSearchResponse(std::string_view raw, SearchResponse* out) {
  if (out == nullptr) return false;
  *out = SearchResponse{};
  // A reply echoes the search target in ST; a NOTIFY announcement carries
  // NT instead, so the ST requirement alone keeps announcements out.
  out->location = fieldValue(raw, "LOCATION");
  out->st = fieldValue(raw, "ST");
  out->usn = fieldValue(raw, "USN");
  return !out->location.empty() && !out->st.empty() && !out->usn.empty();
}

}  // namespace soar::cast
