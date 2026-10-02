#include "soar/core/subtitle_provider.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

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

// The media path behind a MediaSource uri, or "" when the source is not a
// local file (an http stream has no directory to hold sidecars). "file://"
// is stripped; anything without a remote scheme is taken as a path, which
// is how the CLI and the UI both pass local files.
std::string localMediaPath(const std::string& uri) {
  if (uri.rfind("http://", 0) == 0 || uri.rfind("https://", 0) == 0) {
    return {};
  }
  const std::string file_scheme = "file://";
  if (uri.rfind(file_scheme, 0) == 0) {
    return uri.substr(file_scheme.size());
  }
  return uri;
}

std::string toLower(const std::string& s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// External (HTTP) subtitle download. The client is deliberately self-contained
// (http only, one blocking connection per request) rather than shared with
// http_cache.cpp: the two stacks serve different lifetimes — a cache streams
// media for minutes, a subtitle request is one small fetch — and keeping them
// apart means neither can regress the other. See the class comment in
// subtitle_provider.h for the wire protocol this speaks.
// ---------------------------------------------------------------------------

constexpr size_t kHashChunkBytes = 64 * 1024;   // head/tail window of the hash
constexpr size_t kMaxDownloadBytes = 8 * 1024 * 1024;  // search body or subtitle

uint64_t hashWords(const uint8_t* p, size_t n, uint64_t acc) {
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t w = 0;
    for (int b = 7; b >= 0; --b) w = (w << 8) | p[i + b];  // little-endian
    acc += w;
  }
  if (i < n) {  // a tail shorter than a word folds into the low bytes
    uint64_t w = 0;
    for (int b = static_cast<int>(n - i) - 1; b >= 0; --b) {
      w = (w << 8) | p[i + b];
    }
    acc += w;
  }
  return acc;
}

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
  std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
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
    *err = "subtitles: cannot resolve " + u.host + ":" + u.port;
    return -1;
  }
  int fd = -1;
  for (struct addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
    fd = static_cast<int>(socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol));
    if (fd < 0) continue;
#ifdef _WIN32
    if (connect(fd, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) break;
#else
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
#endif
    sockClose(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  if (fd < 0) {
    *err = "subtitles: cannot connect to " + u.hostport;
    return -1;
  }
  // The receive timeout is the caller's budget; it is what turns a server
  // that accepts and never answers into a plain "no candidates".
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
      *err = "subtitles: send failed (errno " + std::to_string(lastNetError()) + ")";
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
      *err = isTimeoutError(e) ? "subtitles: timeout waiting for response headers"
                               : "subtitles: connection closed before response headers";
      return false;
    }
    head->push_back(c);
    if (c == kSep[matched]) {
      if (++matched == 4) return true;
    } else {
      matched = c == kSep[0] ? 1 : 0;
    }
  }
  *err = "subtitles: response headers exceed 64 KiB";
  return false;
}

static bool recvSome(int fd, std::string* body, size_t cap, std::string* err) {
  char buf[16 * 1024];
  for (;;) {
    const int r = static_cast<int>(recv(fd, buf, sizeof(buf), 0));
    if (r < 0) {
      *err = isTimeoutError(lastNetError())
                 ? "subtitles: timeout while receiving body"
                 : "subtitles: receive failed (errno " + std::to_string(lastNetError()) + ")";
      return false;
    }
    if (r == 0) return true;  // server closed: body complete
    body->append(buf, static_cast<size_t>(r));
    if (body->size() > cap) {
      *err = "subtitles: response exceeds " + std::to_string(cap) + " bytes";
      return false;
    }
  }
}

// One GET, the whole body. `api_key` rides along as X-API-Key when set and
// never reaches a log line. Anything but a 2xx answer is a failure — this
// client follows no redirects and speaks no chunked encoding.
static bool httpGet(const std::string& url, const std::string& api_key,
                    std::chrono::milliseconds timeout, std::string* body,
                    std::string* err) {
  err->clear();
  body->clear();
  UrlParts u;
  if (!parseHttpUrl(url, &u)) {
    *err = "subtitles: not a usable http:// url: " + url;
    return false;
  }
  const int fd = connectTcp(u, static_cast<unsigned>(timeout.count()), err);
  if (fd < 0) return false;

  std::string req = "GET " + u.path + " HTTP/1.1\r\nHost: " + u.hostport +
                    "\r\nAccept: */*\r\nUser-Agent: soar-subtitles/1\r\n"
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
    *err = "subtitles: malformed status line";
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
    *err = "subtitles: unexpected status " + std::to_string(status);
    sockClose(fd);
    return false;
  }
  if (has_length && content_length > kMaxDownloadBytes) {
    *err = "subtitles: response of " + std::to_string(content_length) +
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
        *err = "subtitles: connection closed mid-body (" + std::to_string(got) + "/" +
               std::to_string(content_length) + " bytes)";
        break;
      }
      body->append(buf, static_cast<size_t>(r));
      got += static_cast<size_t>(r);
    }
    ok = got == content_length;
  } else {
    ok = recvSome(fd, body, kMaxDownloadBytes, err);
  }
  sockClose(fd);
  return ok;
}

std::string percentEncode(const std::string& s) {
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                            c == '.' || c == '~';
    if (unreserved) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0xf]);
    }
  }
  return out;
}

// One candidate per line, four tab-separated fields (see the header). Bad
// lines are skipped, never fatal — a half-broken catalog still yields its
// parseable half.
std::vector<SubtitleCandidate> parseCandidateLines(const std::string& body) {
  std::vector<SubtitleCandidate> out;
  std::istringstream is(body);
  std::string line;
  while (std::getline(is, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> fields;
    size_t pos = 0;
    while (fields.size() < 5) {
      const size_t tab = line.find('\t', pos);
      if (tab == std::string::npos) {
        fields.push_back(line.substr(pos));
        break;
      }
      fields.push_back(line.substr(pos, tab - pos));
      pos = tab + 1;
    }
    if (fields.size() < 4 || fields[0].empty()) continue;
    if (fields[0].rfind("http://", 0) != 0) continue;  // no TLS client here
    const std::string ext = toLower(fields[3]);
    SubtitleFormat format = SubtitleFormat::Unknown;
    if (ext == "srt" || ext == "subrip") {
      format = SubtitleFormat::SubRip;
    } else if (ext == "vtt" || ext == "webvtt") {
      format = SubtitleFormat::WebVtt;
    } else if (ext == "ass" || ext == "ssa") {
      format = SubtitleFormat::Ass;
    } else if (ext == "ttml" || ext == "dfxp") {
      format = SubtitleFormat::Ttml;
    } else {
      continue;  // only formats the core parser reads are offered
    }
    SubtitleCandidate cand;
    cand.path = fields[0];
    cand.language = fields[1];
    cand.title = fields[2].empty() ? fields[0] : fields[2];
    cand.format = format;
    out.push_back(std::move(cand));
  }
  return out;
}

// Builds "{endpoint}?size=..&hash=..&name=..", appending with & when the
// endpoint already carries a query of its own.
std::string searchUrl(const HttpSubtitleConfig& cfg, const std::string& size,
                      const std::string& hash, const std::string& name) {
  std::string url = cfg.endpoint;
  url += url.find('?') == std::string::npos ? '?' : '&';
  url += "size=" + size + "&hash=" + hash + "&name=" + name;
  return url;
}

} // namespace

std::vector<SubtitleCandidate> SidecarSubtitleProvider::findCandidates(
    const MediaSource& source) const {
  std::vector<SubtitleCandidate> out;

  const std::string local = localMediaPath(source.uri);
  if (local.empty()) {
    return out;
  }
  std::error_code ec;
  const std::filesystem::path media(local);
  // Not a media file (missing, a directory, unreadable): nothing to look for
  // sidecars next to. is_regular_file answers false instead of throwing.
  if (!std::filesystem::is_regular_file(media, ec)) {
    return out;
  }

  const std::string stem = toLower(media.stem().string());
  const std::size_t sep = stem.size();  // a candidate must begin "<stem>."
  const std::filesystem::path dir =
      media.parent_path().empty() ? std::filesystem::path(".") : media.parent_path();
  // The directory is the media's own, so it exists; a permission-denied
  // entry is skipped rather than fatal (skip_permission_denied below).

  std::filesystem::directory_iterator it(
      dir, std::filesystem::directory_options::skip_permission_denied, ec);
  for (const std::filesystem::directory_entry& entry : it) {
    if (!entry.is_regular_file(ec)) {
      continue;  // a directory or a device that merely looks like a sidecar
    }
    const std::string name = entry.path().filename().string();
    const std::string lower = toLower(name);
    // "<stem>" + "." + a non-empty extension, matched case-insensitively.
    if (lower.size() < sep + 2 || lower.compare(0, sep, stem) != 0 ||
        lower[sep] != '.') {
      continue;
    }
    // name[sep] is a dot, so the last one is at or after it: the text
    // between them is the tag ("en", "en.forced", "" when untagged).
    const std::size_t dot = lower.rfind('.');
    const SubtitleFormat format = subtitleFormatFromPath(name);
    if (format == SubtitleFormat::Unknown) {
      continue;  // bitmap subs (PGS, VobSub) need a decoder, not a parser
    }

    SubtitleCandidate cand;
    cand.path = entry.path().string();
    cand.format = format;
    cand.title = name;
    // dot == sep for an untagged file ("movie.srt"), so the tag is empty
    // there rather than the tail of the extension.
    const std::string tag = lower.substr(sep + 1, dot > sep ? dot - sep - 1 : 0);
    // "movie.en.forced.srt" -> language "en"; the rest of the tag ("forced")
    // stays out of the language field.
    cand.language = tag.substr(0, tag.find('.'));
    out.push_back(std::move(cand));
  }

  // Untagged first, then alphabetical: the common single-subtitle file leads
  // the menu and the order never depends on directory iteration order.
  std::sort(out.begin(), out.end(), [](const SubtitleCandidate& a, const SubtitleCandidate& b) {
    const bool a_plain = a.language.empty();
    const bool b_plain = b.language.empty();
    if (a_plain != b_plain) {
      return a_plain;
    }
    return a.title < b.title;
  });
  return out;
}

bool SidecarSubtitleProvider::fetch(const SubtitleCandidate& candidate, std::string& out) const {
  return readSubtitleFile(candidate.path, out);
}

// ---------------------------------------------------------------------------
// ExternalSubtitleProvider
// ---------------------------------------------------------------------------

ExternalSubtitleProvider::ExternalSubtitleProvider(HttpSubtitleConfig config)
    : config_(std::move(config)) {}

void ExternalSubtitleProvider::configure(HttpSubtitleConfig config) {
  config_ = std::move(config);
}

std::vector<SubtitleCandidate> ExternalSubtitleProvider::findCandidates(
    const MediaSource& source) const {
  // Unconfigured means offline: not even a name lookup, per the "no forced
  // networking" rule. The UI shows this as simply "no external subtitles".
  if (config_.endpoint.empty()) {
    return {};
  }
  const std::string local = localMediaPath(source.uri);
  if (local.empty()) {
    return {};  // an http stream has no bytes here to hash
  }
  const std::string hash = mediaHashHex(local);
  if (hash.empty()) {
    return {};  // not a readable file: nothing to identify it by
  }

  std::error_code ec;
  const uintmax_t size = std::filesystem::file_size(local, ec);
  const std::string size_str = ec ? "0" : std::to_string(size);
  const std::string name =
      percentEncode(std::filesystem::path(local).stem().string());

  std::string body;
  std::string err;
  if (!httpGet(searchUrl(config_, size_str, hash, name), config_.api_key,
               config_.timeout, &body, &err)) {
    return {};  // unreachable, timeout, non-2xx: degrade to no candidates
  }
  return parseCandidateLines(body);
}

bool ExternalSubtitleProvider::fetch(const SubtitleCandidate& candidate,
                                     std::string& out) const {
  std::string body;
  std::string err;
  if (!httpGet(candidate.path, config_.api_key, config_.timeout, &body, &err)) {
    return false;
  }
  // Only bytes the core parser genuinely understands count as a subtitle;
  // an HTML error page served with a 200 must not become a track.
  if (detectSubtitleFormat(body) == SubtitleFormat::Unknown) {
    return false;
  }
  out = std::move(body);
  return true;
}

// ---------------------------------------------------------------------------
// mediaHashHex / storeExternalSubtitle
// ---------------------------------------------------------------------------

std::string mediaHashHex(const std::string& media_path) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(media_path, ec)) {
    return {};
  }
  FILE* f = std::fopen(media_path.c_str(), "rb");
  if (!f) {
    return {};
  }
  const uintmax_t size = std::filesystem::file_size(media_path, ec);
  if (ec) {
    std::fclose(f);
    return {};
  }

  // sum of the unsigned 64-bit little-endian words of the head and tail
  // 64 KiB plus the size — the recipe several subtitle services use, so a
  // server can compute the same digest without this client.
  std::vector<uint8_t> buf(kHashChunkBytes);
  uint64_t acc = 0;

  const size_t head = static_cast<size_t>(
      std::min<uintmax_t>(size, kHashChunkBytes));
  if (std::fread(buf.data(), 1, head, f) != head) {
    std::fclose(f);
    return {};
  }
  acc = hashWords(buf.data(), head, acc);

  if (size > kHashChunkBytes) {
    const size_t tail = kHashChunkBytes;
#ifdef _WIN32
    const int seeked = _fseeki64(f, static_cast<__int64>(size - tail), SEEK_SET);
#else
    const int seeked = std::fseek(f, static_cast<long>(size - tail), SEEK_SET);
#endif
    if (seeked != 0 || std::fread(buf.data(), 1, tail, f) != tail) {
      std::fclose(f);
      return {};
    }
    acc = hashWords(buf.data(), tail, acc);
  }
  std::fclose(f);

  acc += size;
  char hex[17];
  std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(acc));
  return hex;
}

std::string storeExternalSubtitle(const std::string& dir,
                                  const SubtitleCandidate& candidate,
                                  const std::string& text) {
  std::error_code ec;
  std::filesystem::path base =
      dir.empty() ? std::filesystem::temp_directory_path(ec)
                  : std::filesystem::path(dir);
  if (dir.empty() && ec) {
    return {};  // not even a temp directory: give up silently
  }

  const std::filesystem::path target = base / "subtitles";
  std::filesystem::create_directories(target, ec);
  if (ec) {
    return {};
  }

  // A title from a remote catalog is untrusted input: anything outside the
  // safe set becomes '_', so it cannot climb out of the subtitles directory.
  std::string name;
  name.reserve(candidate.title.size());
  for (char c : candidate.title) {
    const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    name.push_back(safe ? c : '_');
  }
  if (name.empty() || name == "." || name == "..") {
    name = "subtitle";
  }
  const size_t dot = name.rfind('.');
  if (dot == std::string::npos || dot == 0) {
    name += candidate.format == SubtitleFormat::WebVtt  ? ".vtt"
            : candidate.format == SubtitleFormat::Ass   ? ".ass"
            : candidate.format == SubtitleFormat::Ttml  ? ".ttml"
                                                       : ".srt";
  }

  const std::filesystem::path file = target / name;
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  if (!out) {
    return {};
  }
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  out.flush();
  if (!out) {
    return {};
  }
  return file.string();
}

// ---------------------------------------------------------------------------
// SubtitleTranslator: batch text translation over an OpenAI-compatible chat
// endpoint. Same self-contained blocking client as the download provider
// above — one small request at a time — plus the smallest JSON writing and
// reading the documented request/response shapes need (no JSON library,
// like the rest of the core; the shapes are pinned by the header comment
// and by the fixture-server tests).
// ---------------------------------------------------------------------------

namespace {

// Whitespace as the payload builder sees it: explicit, locale-independent,
// and safe on UTF-8 bytes (a plain isspace(char) would be UB on bytes
// >= 0x80, which every non-ASCII subtitle is full of).
bool isPayloadSpace(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' ||
         ch == '\f' || ch == '\v';
}

// Collapses every whitespace run (cue line breaks included) to one space
// and trims the ends: the wire carries a cue as a single numbered line,
// because a timed caption is one thought. parseSubtitleText never emits an
// empty cue, so this never returns "" for a parsed cue.
std::string collapseLines(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  bool pending_space = false;
  for (char ch : s) {
    if (isPayloadSpace(ch)) {
      pending_space = !out.empty();
    } else {
      if (pending_space) {
        out.push_back(' ');
        pending_space = false;
      }
      out.push_back(ch);
    }
  }
  return out;
}

// Escapes a UTF-8 string as the body of a JSON string (no surrounding
// quotes). Bytes above 0x7f pass through untouched: the payload is valid
// UTF-8 in, valid UTF-8 out.
std::string jsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(ch);
        }
    }
  }
  return out;
}

// Decodes the JSON string literal starting at s[pos] == '"' into `out`.
// False when the literal never terminates or carries an unknown escape.
// \uXXXX escapes are decoded (surrogate pairs included) — endpoints that
// answer ASCII-safe JSON must not leave \uXXXX literal inside translated
// text. A surrogate without a partner becomes U+FFFD, never raw UTF-8 of a
// surrogate value (that would not be valid UTF-8).
bool jsonUnescapeString(const std::string& s, size_t pos, std::string* out) {
  if (pos >= s.size() || s[pos] != '"') {
    return false;
  }
  ++pos;
  out->clear();
  auto hex4 = [&](unsigned* v) {
    if (pos + 4 > s.size()) {
      return false;
    }
    *v = 0;
    for (int i = 0; i < 4; ++i) {
      const char h = s[pos + i];
      *v <<= 4;
      if (h >= '0' && h <= '9') {
        *v |= static_cast<unsigned>(h - '0');
      } else if (h >= 'a' && h <= 'f') {
        *v |= static_cast<unsigned>(h - 'a' + 10);
      } else if (h >= 'A' && h <= 'F') {
        *v |= static_cast<unsigned>(h - 'A' + 10);
      } else {
        return false;
      }
    }
    pos += 4;
    return true;
  };
  auto appendUtf8 = [&](unsigned cp) {
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
  };
  while (pos < s.size()) {
    const char ch = s[pos++];
    if (ch == '"') {
      return true;
    }
    if (ch != '\\') {
      out->push_back(ch);
      continue;
    }
    if (pos >= s.size()) {
      return false;
    }
    const char e = s[pos++];
    switch (e) {
      case '"': out->push_back('"'); break;
      case '\\': out->push_back('\\'); break;
      case '/': out->push_back('/'); break;
      case 'b': out->push_back('\b'); break;
      case 'f': out->push_back('\f'); break;
      case 'n': out->push_back('\n'); break;
      case 'r': out->push_back('\r'); break;
      case 't': out->push_back('\t'); break;
      case 'u': {
        unsigned cp = 0;
        if (!hex4(&cp)) {
          return false;
        }
        if (cp >= 0xD800 && cp <= 0xDBFF && pos + 1 < s.size() &&
            s[pos] == '\\' && s[pos + 1] == 'u') {
          const size_t save = pos;
          pos += 2;
          unsigned lo = 0;
          if (hex4(&lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          } else {
            pos = save;
            cp = 0xFFFD;
          }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
          cp = 0xFFFD;
        }
        appendUtf8(cp);
        break;
      }
      default:
        return false;
    }
  }
  return false;  // unterminated literal
}

// Reads choices[0].message.content from the documented response shape:
// locates "choices", the array, its first object, then "message", then
// "content". Anything missing is not the documented shape and fails the
// translation — the extractor is deliberately not a general JSON parser.
bool chatResponseContent(const std::string& body, std::string* out) {
  size_t p = body.find("\"choices\"");
  if (p == std::string::npos) return false;
  p = body.find('[', p);
  if (p == std::string::npos) return false;
  p = body.find('{', p);
  if (p == std::string::npos) return false;
  p = body.find("\"message\"", p);
  if (p == std::string::npos) return false;
  p = body.find("\"content\"", p);
  if (p == std::string::npos) return false;
  p = body.find(':', p);
  if (p == std::string::npos) return false;
  ++p;
  while (p < body.size() && isPayloadSpace(body[p])) ++p;
  return jsonUnescapeString(body, p, out);
}

// "{endpoint}/chat/completions", tolerating a trailing slash on the
// endpoint and an endpoint that already spells the full path.
std::string chatUrl(const std::string& endpoint) {
  static const char kTail[] = "/chat/completions";
  std::string base = endpoint;
  while (!base.empty() && base.back() == '/') {
    base.pop_back();
  }
  const size_t tail_len = sizeof(kTail) - 1;
  if (base.size() >= tail_len &&
      base.compare(base.size() - tail_len, tail_len, kTail) == 0) {
    return base;
  }
  return base + kTail;
}

// "1. first cue\n2. second cue" — numbering restarts at 1 per batch, and
// the reply is mapped back by position, not by number.
std::string numberedPayload(const std::vector<std::string>& texts) {
  std::string out;
  for (size_t i = 0; i < texts.size(); ++i) {
    if (i > 0) out += '\n';
    out += std::to_string(i + 1);
    out += ". ";
    out += texts[i];
  }
  return out;
}

// Maps the numbered reply lines back to exactly `count` translations, in
// order. False unless every non-blank reply line is numbered ("12.",
// "12)" or "12:") and the count matches — a reply that answers only part
// of the batch must fail the whole translation, never truncate the track.
bool parseNumberedReply(const std::string& content, size_t count,
                        std::vector<std::string>* out) {
  out->clear();
  size_t pos = 0;
  for (;;) {
    const size_t eol = content.find('\n', pos);
    std::string line =
        content.substr(pos, eol == std::string::npos ? eol : eol - pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty()) {
      size_t start = 0;
      while (start < line.size() && line[start] == ' ') ++start;
      size_t digits = start;
      while (digits < line.size() && line[digits] >= '0' &&
             line[digits] <= '9') {
        ++digits;
      }
      if (digits == start) return false;  // not numbered at all
      if (digits >= line.size() ||
          (line[digits] != '.' && line[digits] != ')' &&
           line[digits] != ':')) {
        return false;
      }
      size_t after = digits + 1;
      if (after < line.size() && (line[after] == ' ' || line[after] == '\t')) {
        ++after;
      }
      out->push_back(line.substr(after));
    }
    if (eol == std::string::npos) break;
    pos = eol + 1;
  }
  return out->size() == count;
}

std::string formatTimestamp(std::chrono::milliseconds t, char ms_sep) {
  const long long total = t.count();
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld%c%03lld",
                total / 3600000, (total / 60000) % 60, (total / 1000) % 60,
                ms_sep, total % 1000);
  return buf;
}

// Renders cues back into the input's container format. Cue numbering in
// SubRip follows the file position, so a translated file lines up with
// its source one. Timestamps come from the parser and are non-negative.
std::string serializeSubtitle(const std::vector<SubtitleCue>& cues,
                              SubtitleFormat format) {
  std::string out;
  if (format == SubtitleFormat::WebVtt) {
    out += "WEBVTT\n\n";
  }
  const char ms_sep = format == SubtitleFormat::WebVtt ? '.' : ',';
  for (size_t i = 0; i < cues.size(); ++i) {
    if (format == SubtitleFormat::SubRip) {
      out += std::to_string(i + 1);
      out += '\n';
    }
    out += formatTimestamp(cues[i].begin, ms_sep);
    out += " --> ";
    out += formatTimestamp(cues[i].end, ms_sep);
    out += '\n';
    out += cues[i].text;
    out += "\n\n";
  }
  return out;
}

// One blocking POST with a JSON body; the transport twin of httpGet above
// (no redirects, no chunked encoding, anything but 2xx is a failure). The
// response cap is the same download cap: a translation reply is small, and
// a runaway endpoint must not stream into memory forever.
bool httpPost(const std::string& url, const std::string& api_key,
              const std::string& json_body, std::chrono::milliseconds timeout,
              std::string* body, std::string* err) {
  err->clear();
  body->clear();
  UrlParts u;
  if (!parseHttpUrl(url, &u)) {
    *err = "translate: not a usable http:// url: " + url;
    return false;
  }
  const int fd = connectTcp(u, static_cast<unsigned>(timeout.count()), err);
  if (fd < 0) return false;

  std::string req = "POST " + u.path + " HTTP/1.1\r\nHost: " + u.hostport +
                    "\r\nAccept: application/json\r\n"
                    "User-Agent: soar-subtitles/1\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: " +
                    std::to_string(json_body.size()) +
                    "\r\n"
                    "Connection: close\r\n";
  if (!api_key.empty()) req += "Authorization: Bearer " + api_key + "\r\n";
  req += "\r\n";
  req += json_body;

  std::string head;
  if (!sendAll(fd, req, err) || !recvHeaders(fd, &head, err)) {
    sockClose(fd);
    return false;
  }
  const size_t first_space = head.find(' ');
  if (first_space == std::string::npos) {
    *err = "translate: malformed status line";
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
    *err = "translate: unexpected status " + std::to_string(status);
    sockClose(fd);
    return false;
  }
  if (has_length && content_length > kMaxDownloadBytes) {
    *err = "translate: response of " + std::to_string(content_length) +
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
          fd, buf,
          static_cast<int>(std::min<size_t>(content_length - got, sizeof(buf))),
          0));
      if (r <= 0) {
        *err = "translate: connection closed mid-body (" + std::to_string(got) +
               "/" + std::to_string(content_length) + " bytes)";
        break;
      }
      body->append(buf, static_cast<size_t>(r));
      got += static_cast<size_t>(r);
    }
    ok = got == content_length;
  } else {
    ok = recvSome(fd, body, kMaxDownloadBytes, err);
  }
  sockClose(fd);
  return ok;
}

} // namespace

SubtitleTranslator::SubtitleTranslator(SubtitleTranslateConfig config)
    : config_(std::move(config)) {}

void SubtitleTranslator::configure(SubtitleTranslateConfig config) {
  config_ = std::move(config);
}

bool SubtitleTranslator::translate(const std::string& subtitle_text,
                                   const std::string& target_language,
                                   std::string* out_text,
                                   std::string* err) const {
  if (err != nullptr) err->clear();
  if (out_text != nullptr) out_text->clear();
  // The transport layer writes its reason unconditionally; callers that
  // don't want it still get a safe sink.
  std::string scratch_err;
  std::string* err_sink = err != nullptr ? err : &scratch_err;

  // Unconfigured means offline: no dial, a clear refusal — per the "no
  // forced networking" rule. The model name is part of the configuration:
  // an OpenAI-compatible endpoint needs one, and without it the request
  // shape is not defined.
  if (config_.endpoint.empty() || config_.model.empty()) {
    if (err != nullptr) {
      *err = "translate: not configured (endpoint and model are required)";
    }
    return false;
  }
  if (config_.endpoint.rfind("http://", 0) != 0) {
    if (err != nullptr) {
      *err = "translate: only http:// endpoints are supported: " +
             config_.endpoint;
    }
    return false;
  }
  if (target_language.empty()) {
    if (err != nullptr) {
      *err = "translate: no target language given";
    }
    return false;
  }
  const SubtitleFormat format = detectSubtitleFormat(subtitle_text);
  // Only SubRip/WebVTT are translator input: the wire format is numbered
  // plain-text lines and serializeSubtitle renders back SubRip/WebVTT. An
  // ASS/SSA document or a TTML file would parse into cues but serialize
  // into a container that is not its own — a row that can only mangle.
  if (format != SubtitleFormat::SubRip && format != SubtitleFormat::WebVtt) {
    if (err != nullptr) {
      *err = "translate: input is not SubRip or WebVTT text";
    }
    return false;
  }
  const std::vector<SubtitleCue> cues = parseSubtitleText(subtitle_text, format);
  if (cues.empty()) {
    if (err != nullptr) {
      *err = "translate: input carries no cues";
    }
    return false;
  }

  // The wire text per cue: one line, trimmed. parseSubtitleText never
  // emits an empty cue, so `texts` has exactly one entry per cue and the
  // reply maps back by position.
  std::vector<std::string> texts;
  texts.reserve(cues.size());
  for (const SubtitleCue& cue : cues) {
    texts.push_back(collapseLines(cue.text));
  }

  std::vector<std::string> translated(cues.size());
  const std::string system =
      "You translate subtitle cues. Translate each numbered line into " +
      target_language +
      ". Reply with exactly the same number of numbered lines, in the same "
      "order, translations only - no notes, no originals.";
  const size_t batch = config_.batch_cues < 1 ? 1 : config_.batch_cues;
  for (size_t first = 0; first < texts.size(); first += batch) {
    const size_t last = std::min<size_t>(texts.size(), first + batch);
    const std::vector<std::string> chunk(texts.begin() + static_cast<long>(first),
                                         texts.begin() + static_cast<long>(last));
    std::string payload =
        "{\"model\":\"" + jsonEscape(config_.model) +
        "\",\"temperature\":0,\"messages\":[{\"role\":\"system\",\"content\":\"" +
        jsonEscape(system) + "\"},{\"role\":\"user\",\"content\":\"" +
        jsonEscape(numberedPayload(chunk)) + "\"}]}";

    std::string reply;
    if (!httpPost(chatUrl(config_.endpoint), config_.api_key, payload,
                  config_.timeout, &reply, err_sink)) {
      return false;  // err already carries the transport reason
    }
    std::string content;
    if (!chatResponseContent(reply, &content)) {
      if (err != nullptr) {
        *err = "translate: response carries no choices[0].message.content";
      }
      return false;
    }
    std::vector<std::string> lines;
    if (!parseNumberedReply(content, chunk.size(), &lines)) {
      if (err != nullptr) {
        *err = "translate: reply does not carry one numbered line per cue";
      }
      return false;
    }
    for (size_t j = 0; j < lines.size(); ++j) {
      translated[first + j] = std::move(lines[j]);
    }
  }

  if (out_text != nullptr) {
    std::vector<SubtitleCue> out = cues;
    for (size_t i = 0; i < out.size(); ++i) {
      out[i].text = std::move(translated[i]);
    }
    *out_text = serializeSubtitle(out, format);
  }
  return true;
}

} // namespace soar
