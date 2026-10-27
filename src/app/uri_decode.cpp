#include "uri_decode.h"

#include <cstddef>

namespace soar::app {
namespace {

// ASCII-only lower case: the strings here are launcher-supplied text, and
// std::tolower would need a locale the helper does not care about.
std::string lower_copy(const std::string& s) {
  std::string out = s;
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c += static_cast<char>('a' - 'A');
  }
  return out;
}

bool isHexDigit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return c - 'A' + 10;
}

std::string percentDecode(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c != '%' || i + 2 >= s.size() || !isHexDigit(s[i + 1]) ||
        !isHexDigit(s[i + 2])) {
      // Not a valid escape: keep the character literal so the text still
      // reaches the backend instead of silently dropping bytes.
      out.push_back(c);
      continue;
    }
    out.push_back(static_cast<char>(hexValue(s[i + 1]) * 16 + hexValue(s[i + 2])));
    i += 2;
  }
  return out;
}

}  // namespace

std::string decodeSourceUri(const std::string& uri) {
  constexpr std::size_t kSchemeLen = 7;  // "file://"
  if (lower_copy(uri.substr(0, kSchemeLen)) != "file://") return uri;

  // After the scheme comes an optional authority, then the path. An empty
  // authority or "localhost" is local; anything else is a network host.
  const std::size_t path_start = uri.find('/', kSchemeLen);
  const std::size_t authority_len =
      path_start == std::string::npos ? uri.size() - kSchemeLen : path_start - kSchemeLen;
  const std::string authority = uri.substr(kSchemeLen, authority_len);
  if (!authority.empty() && lower_copy(authority) != "localhost") return uri;

  if (path_start == std::string::npos) return {};
  std::string path = percentDecode(uri.substr(path_start));
#ifndef _WIN32
  return path;
#else
  // file:///C:/a.mkv parses as an empty authority plus the path
  // /C:/a.mkv; Windows needs the drive-root form C:/a.mkv (a leading slash
  // would make the path relative to the current drive's root).
  if (path.size() >= 3 && path[0] == '/' && path[2] == ':') {
    path.erase(0, 1);
  }
  return path;
#endif
}

}  // namespace soar::app
