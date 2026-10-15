#include "soar/core/metadata_scraper.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace soar {

namespace {

// ---------------------------------------------------------------------------
// HTTP transport: one blocking connection, one GET, the whole body - the
// same shape as the external subtitle client (subtitle_provider.cpp). The
// two are deliberately kept apart rather than sharing a transport: the
// subtitle client serves a per-track download and this one a per-entry
// metadata lookup, and neither should be able to regress the other. The
// cost is this duplicated block of socket code, which a future refactor
// could hoist into one internal transport if a third consumer appears.
// ---------------------------------------------------------------------------

// A metadata answer is one search page; a cap of 1 MiB is far past anything
// a service should send for a single query.
constexpr size_t kMaxAnswerBytes = 1 << 20;

struct UrlParts {
  std::string host;
  std::string port;
  std::string hostport;  // as it appeared in the URL, for the Host header
  std::string path;      // absolute path + query
};

bool parseHttpUrl(const std::string& url, UrlParts* out) {
  const std::string scheme = "http://";
  if (url.rfind(scheme, 0) != 0) return false;  // no https: no TLS client here
  std::string rest = url.substr(scheme.size());
  const size_t slash = rest.find('/');
  const std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
  out->path = slash == std::string::npos ? "/" : rest.substr(slash);
  if (authority.empty()) return false;
  std::string host = authority;
  std::string port = "80";
  if (authority[0] == '[') {  // bracketed IPv6 literal: [::1]:8080
    const size_t close = authority.find(']');
    if (close == std::string::npos) return false;
    host = authority.substr(1, close - 1);
    if (close + 1 < authority.size()) {
      if (authority[close + 1] != ':') return false;
      port = authority.substr(close + 2);
    }
  } else {
    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
      host = authority.substr(0, colon);
      port = authority.substr(colon + 1);
    }
  }
  if (host.empty() || port.empty() ||
      port.find_first_not_of("0123456789") != std::string::npos) {
    return false;
  }
  out->host = host;
  out->port = port;
  out->hostport = authority;
  return true;
}

std::string toLower(const std::string& s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

std::string percentEncode(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  char buf[4];
  for (const unsigned char ch : s) {
    // The unreserved set RFC 3986 keeps literal; everything else, including
    // the UTF-8 bytes of a non-ASCII title, is escaped byte by byte.
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
        (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' ||
        ch == '~') {
      out.push_back(static_cast<char>(ch));
    } else {
      std::snprintf(buf, sizeof(buf), "%%%02X", ch);
      out.append(buf);
    }
  }
  return out;
}

#ifdef _WIN32
static void winsockInit() {
  static const bool ready = [] {
    WSADATA d;
    return WSAStartup(MAKEWORD(2, 2), &d) == 0;
  }();
  (void)ready;
}

static void sockClose(int s) { closesocket(s); }
static int lastNetError() { return WSAGetLastError(); }
#else
static void sockClose(int s) { ::close(s); }
static int lastNetError() { return errno; }
#endif

static bool isTimeoutError(int e) {
#ifdef _WIN32
  return e == WSAETIMEDOUT;
#else
  return e == EAGAIN || e == EWOULDBLOCK;
#endif
}

static int connectTcp(const UrlParts& u, unsigned timeout_ms, std::string* err) {
#ifdef _WIN32
  winsockInit();
#endif
  struct addrinfo hints{};
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* res = nullptr;
  const int rc = getaddrinfo(u.host.c_str(), u.port.c_str(), &hints, &res);
  if (rc != 0 || res == nullptr) {
    *err = "metadata: cannot resolve " + u.host + ":" + u.port;
    return -1;
  }
  int fd = -1;
  for (struct addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
    fd = static_cast<int>(socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
    if (fd < 0) continue;
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
    sockClose(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  if (fd < 0) {
    *err = "metadata: cannot connect to " + u.hostport;
    return -1;
  }
  // The receive timeout is the caller's budget; it is what turns a service
  // that accepts and never answers into a plain "no metadata".
#ifdef _WIN32
  const DWORD tv = timeout_ms;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
  struct timeval t{};
  t.tv_sec = static_cast<time_t>(timeout_ms / 1000);
  t.tv_usec = static_cast<suseconds_t>((timeout_ms % 1000) * 1000);
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(t));
#endif
  return fd;
}

static bool sendAll(int fd, const std::string& data, std::string* err) {
  const char* p = data.data();
  size_t n = data.size();
  while (n > 0) {
    const int w =
        static_cast<int>(send(fd, p, static_cast<int>(std::min<size_t>(n, 1 << 20)), 0));
    if (w <= 0) {
      *err = "metadata: send failed (errno " + std::to_string(lastNetError()) + ")";
      return false;
    }
    p += w;
    n -= static_cast<size_t>(w);
  }
  return true;
}

// Reads exactly up to the end of the response headers ("\r\n\r\n" included).
static bool recvHeaders(int fd, std::string* head, std::string* err) {
  head->clear();
  static const char kSep[4] = {'\r', '\n', '\r', '\n'};
  size_t matched = 0;
  while (head->size() < 64 * 1024) {
    char c = 0;
    const int r = static_cast<int>(recv(fd, &c, 1, 0));
    if (r <= 0) {
      const int e = lastNetError();
      *err = isTimeoutError(e) ? "metadata: timeout waiting for response headers"
                               : "metadata: connection closed before response headers";
      return false;
    }
    head->push_back(c);
    if (c == kSep[matched]) {
      if (++matched == 4) return true;
    } else {
      matched = c == kSep[0] ? 1 : 0;
    }
  }
  *err = "metadata: response headers exceed 64 KiB";
  return false;
}

static bool recvSome(int fd, std::string* body, size_t cap, std::string* err) {
  char buf[16 * 1024];
  for (;;) {
    const int r = static_cast<int>(recv(fd, buf, sizeof(buf), 0));
    if (r < 0) {
      *err = isTimeoutError(lastNetError())
                 ? "metadata: timeout while receiving body"
                 : "metadata: receive failed (errno " + std::to_string(lastNetError()) + ")";
      return false;
    }
    if (r == 0) return true;  // server closed: body complete
    body->append(buf, static_cast<size_t>(r));
    if (body->size() > cap) {
      *err = "metadata: response exceeds " + std::to_string(cap) + " bytes";
      return false;
    }
  }
}

// One GET, the whole body. `api_key` rides along as X-API-Key when set and
// never reaches a log line. Anything but a 2xx answer is a failure - this
// client follows no redirects and speaks no chunked encoding.
static bool httpGet(const std::string& url, const std::string& api_key,
                    std::chrono::milliseconds timeout, std::string* body,
                    std::string* err) {
  err->clear();
  body->clear();
  UrlParts u;
  if (!parseHttpUrl(url, &u)) {
    *err = "metadata: not a usable http:// url: " + url;
    return false;
  }
  const int fd = connectTcp(u, static_cast<unsigned>(timeout.count()), err);
  if (fd < 0) return false;

  std::string req = "GET " + u.path + " HTTP/1.1\r\nHost: " + u.hostport +
                    "\r\nAccept: application/json\r\nUser-Agent: soar-metadata/1\r\n"
                    "Connection: close\r\n";
  if (!api_key.empty()) req += "X-API-Key: " + api_key + "\r\n";
  req += "\r\n";

  std::string head;
  if (!sendAll(fd, req, err) || !recvHeaders(fd, &head, err)) {
    sockClose(fd);
    return false;
  }
  const size_t first_space = head.find(' ');
  if (first_space == std::string::npos) {
    *err = "metadata: malformed status line";
    sockClose(fd);
    return false;
  }
  const int status = std::atoi(head.c_str() + first_space + 1);

  size_t content_length = 0;
  bool has_length = false;
  {
    size_t line = head.find("\r\n");
    line = line == std::string::npos ? std::string::npos : line + 2;
    while (line != std::string::npos && line < head.size()) {
      const size_t eol = head.find("\r\n", line);
      const std::string h = toLower(head.substr(
          line, (eol == std::string::npos ? head.size() : eol) - line));
      if (h.rfind("content-length:", 0) == 0) {
        const long v = std::atol(h.c_str() + 15);
        if (v >= 0) {
          content_length = static_cast<size_t>(v);
          has_length = true;
        }
      }
      line = eol == std::string::npos ? std::string::npos : eol + 2;
    }
  }

  if (status < 200 || status >= 300) {
    *err = "metadata: unexpected status " + std::to_string(status);
    sockClose(fd);
    return false;
  }
  if (has_length && content_length > kMaxAnswerBytes) {
    *err = "metadata: response of " + std::to_string(content_length) +
           " bytes exceeds the download cap";
    sockClose(fd);
    return false;
  }

  bool ok;
  if (has_length) {
    body->reserve(content_length);
    char buf[16 * 1024];
    size_t got = 0;
    while (got < content_length) {
      const int r = static_cast<int>(recv(
          fd, buf, static_cast<int>(std::min<size_t>(content_length - got, sizeof(buf))), 0));
      if (r <= 0) {
        *err = "metadata: connection closed mid-body (" + std::to_string(got) + "/" +
               std::to_string(content_length) + " bytes)";
        break;
      }
      body->append(buf, static_cast<size_t>(r));
      got += static_cast<size_t>(r);
    }
    ok = got == content_length;
  } else {
    ok = recvSome(fd, body, kMaxAnswerBytes, err);
  }
  sockClose(fd);
  return ok;
}

// ---------------------------------------------------------------------------
// A just-enough JSON reader. The answer is an object whose "results" array
// holds objects, and the scraper wants at most two string fields from it.
// There is no JSON library in this codebase, so this walks the bytes
// directly: string literals are consumed whole (a key that merely appears
// inside a value cannot be mistaken for a key), objects and arrays are
// walked depth first, and primitives are skipped.
// ---------------------------------------------------------------------------

bool isWs(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

void skipWs(const std::string& s, size_t& i) {
  while (i < s.size() && isWs(s[i])) ++i;
}

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Encodes a Unicode code point as UTF-8, whatever its width.
void appendCodePoint(std::string* out, int cp) {
  if (cp < 0x80) {
    out->push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

// Reads one JSON string literal at s[i] == '"'. UTF-8 bytes pass through
// unchanged; \uXXXX escapes are decoded, surrogate pairs are re-encoded, and
// a lone surrogate becomes U+FFFD rather than invalid UTF-8. Returns false
// on a truncated or malformed literal.
bool readJsonString(const std::string& s, size_t& i, std::string* out) {
  if (i >= s.size() || s[i] != '"') return false;
  ++i;
  out->clear();
  for (;;) {
    if (i >= s.size()) return false;
    const char c = s[i];
    if (c == '"') {
      ++i;
      return true;
    }
    if (c != '\\') {
      out->push_back(c);
      ++i;
      continue;
    }
    ++i;
    if (i >= s.size()) return false;
    switch (s[i]) {
      case '"': out->push_back('"'); ++i; break;
      case '\\': out->push_back('\\'); ++i; break;
      case '/': out->push_back('/'); ++i; break;
      case 'b': out->push_back('\b'); ++i; break;
      case 'f': out->push_back('\f'); ++i; break;
      case 'n': out->push_back('\n'); ++i; break;
      case 'r': out->push_back('\r'); ++i; break;
      case 't': out->push_back('\t'); ++i; break;
      case 'u': {
        if (i + 5 > s.size()) return false;
        int v = 0;
        bool bad = false;
        for (int k = 0; k < 4; ++k) {
          const int d = hexDigit(s[i + 1 + k]);
          if (d < 0) {
            bad = true;
            break;
          }
          v = v * 16 + d;
        }
        if (bad) return false;
        i += 5;
        // A high surrogate may be followed by a low one: the pair is a
        // single code point above the BMP. Every other surrogate shape - a
        // lone high with no escape behind it, a lone low - has no code point
        // of its own and becomes U+FFFD, never a raw surrogate byte.
        int cp = v;
        if (v >= 0xD800 && v <= 0xDBFF && i + 6 <= s.size() && s[i] == '\\' &&
            s[i + 1] == 'u') {
          int lo = 0;
          bool badlo = false;
          for (int k = 0; k < 4; ++k) {
            const int d = hexDigit(s[i + 2 + k]);
            if (d < 0) {
              badlo = true;
              break;
            }
            lo = lo * 16 + d;
          }
          if (badlo) return false;
          if (lo >= 0xDC00 && lo <= 0xDFFF) {
            cp = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
            i += 6;
          } else {
            cp = 0xFFFD;  // not a low surrogate: the high one stands alone
          }
        } else if (v >= 0xD800 && v <= 0xDFFF) {
          cp = 0xFFFD;
        }
        appendCodePoint(out, cp);
        break;
      }
      default:
        return false;  // an escape the JSON vocabulary does not define
    }
  }
}

// Skips one JSON value, or finds `key` inside it. Returns true once the key
// is found, false when the whole value has been consumed without it.
bool consumeValue(const std::string& s, size_t& i, const std::string& key, std::string* out) {
  skipWs(s, i);
  if (i >= s.size()) return false;
  const char c = s[i];
  if (c == '"') {
    std::string lit;
    readJsonString(s, i, &lit);  // a plain string: skip it, never a match
    return false;
  }
  if (c == '{') {
    ++i;
    for (;;) {
      skipWs(s, i);
      if (i >= s.size()) return false;
      // Either closer ends this container: a ']' here means the object was
      // malformed (an array's terminator), and stopping the walk there keeps
      // the enclosing loops from re-entering on the same byte forever.
      if (s[i] == '}' || s[i] == ']') {
        ++i;
        return false;
      }
      if (s[i] != '"') return false;
      std::string name;
      if (!readJsonString(s, i, &name)) return false;
      skipWs(s, i);
      if (i >= s.size() || s[i] != ':') return false;
      ++i;
      if (name == key) {
        skipWs(s, i);
        if (i >= s.size()) return false;
        if (s[i] == '"') return readJsonString(s, i, out);
        // The key carries a non-string value: skip it and keep looking.
        if (consumeValue(s, i, key, out)) return true;
        skipWs(s, i);
        if (i < s.size() && s[i] == ',') ++i;
        continue;
      }
      if (consumeValue(s, i, key, out)) return true;
      skipWs(s, i);
      if (i < s.size() && s[i] == ',') ++i;
    }
  }
  if (c == '[') {
    ++i;
    for (;;) {
      skipWs(s, i);
      if (i >= s.size()) return false;
      // Either closer ends this container, mirroring the object loop: a
      // member the reader rejected can leave the walk on an inner '}', and
      // without this the loop would hand the same byte back forever.
      if (s[i] == ']' || s[i] == '}') {
        ++i;
        return false;
      }
      if (consumeValue(s, i, key, out)) return true;
      skipWs(s, i);
      if (i < s.size() && s[i] == ',') ++i;
    }
  }
  // A number, a literal, or a delimiter: walk off the token.
  while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' && !isWs(s[i])) ++i;
  return false;
}

// The first string-valued `key` anywhere in `s`, or false.
bool jsonStringField(const std::string& s, const std::string& key, std::string* out) {
  size_t i = 0;
  return consumeValue(s, i, key, out);
}

}  // namespace

HttpMetadataScraper::HttpMetadataScraper(HttpMetadataConfig config)
    : config_(std::move(config)) {}

void HttpMetadataScraper::configure(HttpMetadataConfig config) {
  config_ = std::move(config);
}

bool HttpMetadataScraper::online() const {
  // https:// is not honored: the client speaks no TLS, so offering the
  // scrape would only fail every attempt.
  return config_.endpoint.rfind("http://", 0) == 0;
}

bool HttpMetadataScraper::scrape(const std::string& query, ScrapedMetadata* out,
                                 std::string* err) const {
  if (err) err->clear();
  if (!online()) {
    if (err) *err = "metadata: no http:// endpoint configured";
    return false;
  }
  std::string body;
  std::string e;
  const std::string url = config_.endpoint + "/search/movie?query=" + percentEncode(query);
  if (!httpGet(url, config_.api_key, config_.timeout, &body, &e)) {
    if (err) *err = e;
    return false;
  }
  std::string title;
  if (!jsonStringField(body, "title", &title) || title.empty()) {
    if (err) *err = "metadata: the answer carries no title";
    return false;
  }
  std::string plot;
  jsonStringField(body, "overview", &plot);  // optional: no plot is a hit anyway
  if (out) *out = ScrapedMetadata{title, plot};
  return true;
}

}  // namespace soar
