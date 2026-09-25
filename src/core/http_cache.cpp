#include "soar/core/http_cache.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <direct.h>
#  include <fcntl.h>
#  include <io.h>
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <fcntl.h>
#  include <netdb.h>
#  include <sys/socket.h>
#  include <sys/stat.h>
#  include <sys/time.h>
#  include <unistd.h>
#endif

namespace soar {

namespace {

// On-disk layout (see the class comment in http_cache.h).
constexpr uint32_t kBlockSize = 256 * 1024;
constexpr uint8_t kMagic[8] = {'S', 'O', 'A', 'R', 'C', 'H', 'N', '1'};
constexpr uint32_t kMetaVersion = 1;
// Hard stop for a dead server: the FFmpeg watchdog cannot interrupt a recv
// happening inside this component (it only polls FFmpeg's own loops), so the
// socket timeout is the only thing preventing an infinite block.
constexpr int kRecvTimeoutSec = 60;
constexpr size_t kMaxHeaderBytes = 64 * 1024;
constexpr size_t kMaxMetaBytes = 4 * 1024 * 1024;  // 2 KiB per GB cached

uint64_t fnv1a64(const std::string& s) {
  uint64_t h = 14695981039346656037ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

std::string hex16(uint64_t v) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
  return buf;
}

void putU32(std::string* s, uint32_t v) {
  for (int i = 0; i < 4; ++i) s->push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

void putU64(std::string* s, uint64_t v) {
  for (int i = 0; i < 8; ++i) s->push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

uint32_t getU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t getU64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

std::string errnoText() {
#ifdef _WIN32
  (void)errno;
  return std::to_string(errno);  // CRT errno as a number; good enough for a message
#else
  return std::strerror(errno);
#endif
}

// Creates dir and any missing parents. Failure is tolerated here (another
// process may have created it); the data-file open is what actually checks
// writability.
void ensureDir(const std::string& dir) {
  for (size_t i = 1; i <= dir.size(); ++i) {
    if (i == dir.size() || dir[i] == '/') {
#ifdef _WIN32
      _mkdir(dir.substr(0, i).c_str());
#else
      ::mkdir(dir.substr(0, i).c_str(), 0755);
#endif
    }
  }
}

bool readWholeFile(const std::string& path, std::string* out, size_t max_bytes) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  bool ok = false;
  char buf[4096];
  size_t total = 0;
  size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
    total += n;
    if (total > max_bytes) break;
    out->append(buf, n);
  }
  ok = n == 0 && total <= max_bytes && std::feof(f) != 0;
  std::fclose(f);
  return ok;
}

// ---------------------------------------------------------------------------
// Minimal blocking HTTP client (http only, one connection per request).
// ---------------------------------------------------------------------------

struct UrlParts {
  std::string host;
  std::string port;
  std::string hostport;  // as it appeared in the URL, for the Host header
  std::string path;      // absolute path + query
};

bool parseHttpUrl(const std::string& url, UrlParts* out) {
  const std::string scheme = "http://";
  if (url.rfind(scheme, 0) != 0) return false;
  std::string rest = url.substr(scheme.size());
  const size_t slash = rest.find('/');
  std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
  out->path = slash == std::string::npos ? "/" : rest.substr(slash);
  if (authority.empty()) return false;
  std::string host = authority;
  std::string port = "80";
  // Bracketed IPv6 literal: [::1]:8080
  if (!authority.empty() && authority[0] == '[') {
    const size_t close = authority.find(']');
    if (close == std::string::npos) return false;
    host = authority.substr(0, close + 1);
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

struct HttpResponse {
  int status = 0;
  bool has_range = false;                       // valid Content-Range present
  uint64_t range_first = 0, range_last = 0, range_total = 0;
  std::vector<uint8_t> body;
};

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

static int connectTcp(const UrlParts& u, std::string* err) {
#ifdef _WIN32
  winsockInit();
#endif
  struct addrinfo hints{};
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* res = nullptr;
  const int rc = getaddrinfo(u.host.c_str(), u.port.c_str(), &hints, &res);
  if (rc != 0 || res == nullptr) {
    *err = "http: cannot resolve " + u.host + ":" + u.port +
           (rc != 0 ? " (code " + std::to_string(rc) + ")" : "");
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
    *err = "http: cannot connect to " + u.hostport;
    return -1;
  }
#ifdef _WIN32
  const DWORD timeout_ms = kRecvTimeoutSec * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms),
             sizeof(timeout_ms));
#else
  struct timeval tv{};
  tv.tv_sec = kRecvTimeoutSec;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
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
      *err = "http: send failed (errno " + std::to_string(lastNetError()) + ")";
      return false;
    }
    p += w;
    n -= static_cast<size_t>(w);
  }
  return true;
}

// Reads exactly up to the end of the response headers ("\r\n\r\n" included).
static bool recvHeaders(int fd, std::vector<uint8_t>* head, std::string* err) {
  head->clear();
  static const uint8_t kSep[4] = {'\r', '\n', '\r', '\n'};
  size_t matched = 0;
  while (head->size() < kMaxHeaderBytes) {
    uint8_t c = 0;
    const int r = static_cast<int>(recv(fd, reinterpret_cast<char*>(&c), 1, 0));
    if (r <= 0) {
      const int e = lastNetError();
      *err = isTimeoutError(e) ? "http: timeout waiting for response headers"
                               : "http: connection closed before response headers";
      return false;
    }
    head->push_back(c);
    if (c == kSep[matched]) {
      if (++matched == 4) return true;
    } else {
      matched = c == kSep[0] ? 1 : 0;
    }
  }
  *err = "http: response headers exceed 64 KiB";
  return false;
}

static bool recvExact(int fd, size_t n, std::vector<uint8_t>* body, std::string* err) {
  body->resize(n);
  size_t got = 0;
  while (got < n) {
    const int r = static_cast<int>(recv(
        fd, reinterpret_cast<char*>(body->data() + got),
        static_cast<int>(std::min<size_t>(n - got, 1 << 20)), 0));
    if (r <= 0) {
      const int e = lastNetError();
      *err = isTimeoutError(e)
                 ? "http: timeout while receiving body (" + std::to_string(got) + "/" +
                       std::to_string(n) + " bytes)"
                 : "http: connection closed mid-body (" + std::to_string(got) + "/" +
                       std::to_string(n) + " bytes)";
      return false;
    }
    got += static_cast<size_t>(r);
  }
  return true;
}

static std::string toLower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

// Parses "bytes 100-299/5000" (the only Content-Range form we accept).
static bool parseContentRange(const std::string& value, HttpResponse* r) {
  const std::string prefix = "bytes ";
  if (value.rfind(prefix, 0) != 0) return false;
  std::istringstream is(value.substr(prefix.size()));
  uint64_t first = 0;
  uint64_t last = 0;
  uint64_t total = 0;
  char dash = 0;
  char slash = 0;
  if (!(is >> first >> dash >> last >> slash >> total)) return false;
  if (dash != '-' || slash != '/') return false;
  r->has_range = true;
  r->range_first = first;
  r->range_last = last;
  r->range_total = total;
  return true;
}

// Issues one GET and reads the complete body. `range` is the full header
// value (e.g. "bytes=0-99") or empty for a plain request.
static bool httpFetch(const std::string& url, const std::string& range,
                      HttpResponse* out, std::string* err) {
  err->clear();
  UrlParts u;
  if (!parseHttpUrl(url, &u)) {
    *err = "http: not a usable http:// url: " + url;
    return false;
  }
  const int fd = connectTcp(u, err);
  if (fd < 0) return false;

  std::string req = "GET " + u.path + " HTTP/1.1\r\nHost: " + u.hostport +
                    "\r\nAccept: */*\r\nUser-Agent: soar-http-cache/1\r\n"
                    "Connection: close\r\n";
  if (!range.empty()) req += "Range: " + range + "\r\n";
  req += "\r\n";

  std::vector<uint8_t> head;
  if (!sendAll(fd, req, err) || !recvHeaders(fd, &head, err)) {
    sockClose(fd);
    return false;
  }

  const std::string text(head.begin(), head.end());
  const size_t first_space = text.find(' ');
  if (first_space == std::string::npos) {
    *err = "http: malformed status line";
    sockClose(fd);
    return false;
  }
  out->status = std::atoi(text.c_str() + first_space + 1);

  bool chunked = false;
  uint64_t content_length = 0;
  bool has_content_range = false;
  {
    size_t line = text.find("\r\n");
    line = line == std::string::npos ? std::string::npos : line + 2;
    while (line != std::string::npos && line < text.size()) {
      const size_t eol = text.find("\r\n", line);
      const size_t end = eol == std::string::npos ? text.size() : eol;
      const std::string h = toLower(text.substr(line, end - line));
      if (h.rfind("content-length:", 0) == 0) {
        content_length = std::strtoull(h.c_str() + 15, nullptr, 10);
      } else if (h.rfind("transfer-encoding:", 0) == 0) {
        if (h.find("chunked") != std::string::npos) chunked = true;
      } else if (h.rfind("content-range:", 0) == 0) {
        has_content_range = parseContentRange(h.c_str() + 14, out);
      }
      line = eol == std::string::npos ? std::string::npos : eol + 2;
    }
  }

  if (out->status >= 300 && out->status < 400) {
    *err = "http: redirects are not supported (status " + std::to_string(out->status) + ")";
    sockClose(fd);
    return false;
  }
  if (chunked) {
    *err = "http: chunked transfer encoding is not supported";
    sockClose(fd);
    return false;
  }
  if (out->status < 200 || out->status >= 300) {
    *err = "http: unexpected status " + std::to_string(out->status);
    sockClose(fd);
    return false;
  }
  (void)has_content_range;

  const bool ok = recvExact(fd, static_cast<size_t>(content_length), &out->body, err);
  sockClose(fd);
  return ok;
}

// ---------------------------------------------------------------------------
// Positional file IO (POSIX fd / Windows CRT handle).
// ---------------------------------------------------------------------------

static bool fdWriteAllAt(int fd, const uint8_t* p, size_t n, uint64_t off,
                         std::string* err) {
  while (n > 0) {
#ifdef _WIN32
    if (_lseeki64(fd, static_cast<__int64>(off), SEEK_SET) < 0) {
      *err = "http cache: seek in data file failed: " + errnoText();
      return false;
    }
    const int w = _write(fd, p, static_cast<unsigned>(std::min<size_t>(n, 1u << 30)));
#else
    const ssize_t w = pwrite(fd, p, std::min<size_t>(n, 1u << 30),
                             static_cast<off_t>(off));
#endif
    if (w <= 0) {
      *err = "http cache: write to data file failed: " + errnoText();
      return false;
    }
    p += w;
    n -= static_cast<size_t>(w);
    off += static_cast<uint64_t>(w);
  }
  return true;
}

static bool fdReadAllAt(int fd, uint8_t* p, size_t n, uint64_t off, std::string* err) {
  while (n > 0) {
#ifdef _WIN32
    if (_lseeki64(fd, static_cast<__int64>(off), SEEK_SET) < 0) {
      *err = "http cache: seek in data file failed: " + errnoText();
      return false;
    }
    const int r = _read(fd, p, static_cast<unsigned>(std::min<size_t>(n, 1u << 30)));
#else
    const ssize_t r = pread(fd, p, std::min<size_t>(n, 1u << 30),
                            static_cast<off_t>(off));
#endif
    if (r <= 0) {
      *err = "http cache: read from data file failed: " + errnoText();
      return false;
    }
    p += r;
    n -= static_cast<size_t>(r);
    off += static_cast<uint64_t>(r);
  }
  return true;
}

static bool fdTruncate(int fd, uint64_t size, std::string* err) {
#ifdef _WIN32
  if (_chsize_s(fd, static_cast<__int64>(size)) != 0) {
#else
  if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
#endif
    *err = "http cache: cannot size data file to " + std::to_string(size) + ": " +
           errnoText();
    return false;
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// HttpCache
// ---------------------------------------------------------------------------

HttpCache::HttpCache(std::string cache_dir, std::string url)
    : cache_dir_(std::move(cache_dir)), url_(std::move(url)) {
  if (url_.rfind("http://", 0) != 0) {
    last_error_ = "http cache: only http:// urls use the cache, got: " + url_;
    return;
  }
  if (cache_dir_.empty()) {
    last_error_ = "http cache: cache dir is empty";
    return;
  }
  ensureDir(cache_dir_);
  openFiles();

  // Probe the source size; this also tells reachable hosts apart from
  // servers that ignore Range (they answer 200 with the whole file). A
  // failed probe is not fatal when a matching meta exists: the cache then
  // opens offline from the meta (replay of a fully cached source).
  HttpResponse probe;
  std::string probe_err;
  if (httpFetch(url_, "bytes=0-0", &probe, &probe_err) && probe.status == 206 &&
      probe.has_range) {
    size_probed_ = true;
    size_ = probe.range_total;
  }

  if (size_probed_) {
    block_count_ = static_cast<uint32_t>((size_ + kBlockSize - 1) / kBlockSize);
    if (!loadMeta()) rebuildMeta();
  } else if (!loadMeta()) {
    last_error_ = "http cache: source unreachable and no usable cache for " + url_ +
                  ": " + probe_err;
    return;
  }

#ifdef _WIN32
  data_fd_ = _open(data_path_.c_str(), _O_RDWR | _O_CREAT | _O_BINARY,
                   _S_IREAD | _S_IWRITE);
#else
  data_fd_ = ::open(data_path_.c_str(), O_RDWR | O_CREAT, 0644);
#endif
  if (data_fd_ < 0) {
    last_error_ = "http cache: cannot open data file: " + errnoText();
    return;
  }
  // Idempotent: no-op when the file already has exactly this size, heals a
  // file left short by a mid-write crash. Cached bytes beyond a smaller size
  // would only exist in blocks the bitmap lost, which loadMeta already
  // rejected.
  if (!fdTruncate(data_fd_, size_, &last_error_)) return;
  valid_ = true;
}

HttpCache::~HttpCache() {
#ifdef _WIN32
  if (data_fd_ >= 0) _close(data_fd_);
#else
  if (data_fd_ >= 0) ::close(data_fd_);
#endif
}

void HttpCache::openFiles() {
  const std::string stem = hex16(fnv1a64(url_));
  meta_path_ = cache_dir_ + "/" + stem + ".meta";
  data_path_ = cache_dir_ + "/" + stem + ".data";
}

void HttpCache::rebuildMeta() {
  bitmap_.assign((block_count_ + 7) / 8, 0);
  saveMeta();
}

bool HttpCache::loadMeta() {
  std::string m;
  if (!readWholeFile(meta_path_, &m, kMaxMetaBytes)) return false;
  // magic(8) + version(4) + url_len(4) + url + size(8) + block_count(4) + bitmap
  if (m.size() < 20) return false;
  const auto* p = reinterpret_cast<const uint8_t*>(m.data());
  if (std::memcmp(p, kMagic, sizeof(kMagic)) != 0) return false;
  if (getU32(p + 8) != kMetaVersion) return false;
  const uint32_t url_len = getU32(p + 12);
  if (m.size() < 20 + url_len + 12) return false;
  if (std::memcmp(p + 20, url_.data(), url_len) != 0 ||
      url_.size() != url_len) {
    return false;  // a different source was cached under this name
  }
  const auto* q = p + 20 + url_len;
  const uint64_t meta_size = getU64(q);
  const uint32_t meta_blocks = getU32(q + 8);
  if (size_probed_) {
    // The server wins: a meta describing a different size is stale.
    if (meta_size != size_) return false;
  } else {
    size_ = meta_size;  // offline: the meta is the only source of truth
  }
  block_count_ = static_cast<uint32_t>((size_ + kBlockSize - 1) / kBlockSize);
  if (meta_blocks != block_count_) return false;
  std::vector<uint8_t> bitmap(m.begin() + static_cast<long>(20 + url_len + 12), m.end());
  if (bitmap.size() != (block_count_ + 7) / 8) return false;
  bitmap_ = std::move(bitmap);
  return true;
}

bool HttpCache::saveMeta() {
  std::string m;
  m.append(reinterpret_cast<const char*>(kMagic), sizeof(kMagic));
  putU32(&m, kMetaVersion);
  putU32(&m, static_cast<uint32_t>(url_.size()));
  m.append(url_);
  putU64(&m, size_);
  putU32(&m, block_count_);
  m.append(reinterpret_cast<const char*>(bitmap_.data()), bitmap_.size());

  const std::string tmp = meta_path_ + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (f == nullptr) {
    last_error_ = "http cache: cannot write meta file: " + errnoText();
    return false;
  }
  const size_t w = std::fwrite(m.data(), 1, m.size(), f);
  const bool ok = w == m.size() && std::fclose(f) == 0;
  if (!ok) {
    last_error_ = "http cache: short write to meta file";
    std::remove(tmp.c_str());
    return false;
  }
  std::remove(meta_path_.c_str());
  if (std::rename(tmp.c_str(), meta_path_.c_str()) != 0) {
    last_error_ = "http cache: cannot finalize meta file: " + errnoText();
    return false;
  }
  return true;
}

uint64_t HttpCache::cachedBytes() const {
  uint64_t bytes = 0;
  for (uint32_t i = 0; i < block_count_; ++i) {
    if ((bitmap_[i >> 3] & (0x80u >> (i & 7))) != 0) {
      bytes += std::min<uint64_t>(kBlockSize, size_ - static_cast<uint64_t>(i) * kBlockSize);
    }
  }
  return bytes;
}

bool HttpCache::fetchRange(uint64_t begin, uint32_t len, std::vector<uint8_t>* out) {
  HttpResponse r;
  const std::string range =
      "bytes=" + std::to_string(begin) + "-" + std::to_string(begin + len - 1);
  if (!httpFetch(url_, range, &r, &last_error_)) return false;
  if (r.status != 206 || !r.has_range) {
    last_error_ = "http cache: expected 206 Partial Content for a range fetch (status " +
                  std::to_string(r.status) + ")";
    return false;
  }
  if (r.range_first != begin || r.range_last != begin + len - 1) {
    last_error_ = "http cache: server returned a different range than requested";
    return false;
  }
  if (r.range_total != size_) {
    last_error_ = "http cache: source size changed (cache " + std::to_string(size_) +
                  ", server " + std::to_string(r.range_total) + ")";
    return false;
  }
  if (r.body.size() != len) {
    last_error_ = "http cache: short range body (" + std::to_string(r.body.size()) +
                  " of " + std::to_string(len) + " bytes)";
    return false;
  }
  *out = std::move(r.body);
  return true;
}

bool HttpCache::fetchBlock(uint32_t block) {
  const uint64_t begin = static_cast<uint64_t>(block) * kBlockSize;
  const uint32_t len =
      static_cast<uint32_t>(std::min<uint64_t>(kBlockSize, size_ - begin));
  std::vector<uint8_t> buf;
  if (!fetchRange(begin, len, &buf)) return false;
  if (!fdWriteAllAt(data_fd_, buf.data(), buf.size(), begin, &last_error_)) return false;
  bitmap_[block >> 3] |= static_cast<uint8_t>(0x80u >> (block & 7));
  return saveMeta();
}

bool HttpCache::ensureBlock(uint32_t block) {
  if ((bitmap_[block >> 3] & (0x80u >> (block & 7))) != 0) return true;
  return fetchBlock(block);
}

size_t HttpCache::read(uint64_t offset, uint8_t* dst, size_t len) {
  last_error_.clear();
  if (!valid_) {
    last_error_ = "http cache: instance is not valid";
    return npos;
  }
  if (offset >= size_) return 0;
  const uint64_t avail = std::min<uint64_t>(len, size_ - offset);
  uint64_t pos = offset;
  size_t done = 0;
  while (done < avail) {
    const uint64_t block = pos / kBlockSize;
    if (!ensureBlock(static_cast<uint32_t>(block))) {
      if (last_error_.empty()) last_error_ = "http cache: block fetch failed";
      return npos;
    }
    const uint64_t block_begin = block * kBlockSize;
    const size_t chunk = static_cast<size_t>(
        std::min<uint64_t>(avail - done, kBlockSize - (pos - block_begin)));
    if (!fdReadAllAt(data_fd_, dst + done, chunk, pos, &last_error_)) return npos;
    done += chunk;
    pos += chunk;
  }
  return static_cast<size_t>(done);
}

bool HttpCache::fetchAll() {
  for (uint32_t b = 0; b < block_count_; ++b) {
    if (!ensureBlock(b)) return false;
  }
  return true;
}

}  // namespace soar
