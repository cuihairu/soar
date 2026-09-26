// Tests for the http:// disk cache (P3a): component-level behavior against
// a real Range server (exact bytes, hole semantics, meta rebuild, resume
// basis, offline reopen), plus one FFmpeg integration case that plays
// through the cache and replays the same url after the server is killed.
//
// Every case needs POSIX fork + python3, so Windows builds report a skip
// message and return; the suite binary exists on every platform.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "soar/core/http_cache.h"
#include "test_http_servers.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using test_servers::kPlainServerScript;
using test_servers::kRangeServerScript;
#ifndef _WIN32
// POSIX-only fixtures: these symbols live inside the shared header's
// #ifndef _WIN32 block, so the using-declarations must be gated too.
using test_servers::RangeServer;
using test_servers::startRangeServer;
using test_servers::startRangeServerV6;
using test_servers::startRawServer;
using test_servers::startPipedServer;
#endif
namespace {

constexpr size_t kBlockSize = 256 * 1024;

#ifndef _WIN32

// Same hash the implementation uses to derive cache file names; tests need
// it only to copy one cache entry onto another url's file names.
uint64_t fnv1a64(const std::string& s) {
  uint64_t h = 14695981039346656037ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

std::string cacheStem(const std::string& url) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx",
                static_cast<unsigned long long>(fnv1a64(url)));
  return buf;
}

// Unique per-process scratch dir; removed by the caller's guard.
std::string makeTempDir(const char* tag) {
  std::string tmpl = std::string("/tmp/soar_cache_") + tag + "_XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  const char* dir = ::mkdtemp(buf.data());
  return dir ? std::string(dir) : std::string();
}

void removeTree(const std::string& dir) {
  // Test scratch dirs contain only flat cache/source files.
  std::string cmd = "rm -rf '" + dir + "'";
  (void)std::system(cmd.c_str());
}

// Deterministic pseudo-random source file (compression-hostile).
std::string makeSourceFile(const std::string& dir, const std::string& name,
                           size_t size) {
  std::string path = dir + "/" + name;
  FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) return std::string();
  std::mt19937 rng(0x50415253u);  // "SARS"-ish constant, fixed seed
  std::vector<uint8_t> buf(size);
  for (size_t i = 0; i < size; ++i) {
    buf[i] = static_cast<uint8_t>(rng() & 0xff);
  }
  const size_t w = std::fwrite(buf.data(), 1, buf.size(), f);
  std::fclose(f);
  return w == size ? path : std::string();
}

std::string readSourceSlice(const std::string& path, size_t offset, size_t len) {
  FILE* f = std::fopen(path.c_str(), "rb");
  REQUIRE(f != nullptr);
  std::vector<char> buf(len);
  REQUIRE(std::fseek(f, static_cast<long>(offset), SEEK_SET) == 0);
  const size_t got = std::fread(buf.data(), 1, len, f);
  std::fclose(f);
  REQUIRE(got == len);
  return std::string(buf.data(), buf.size());
}

void writeBytes(const std::string& path, const std::string& content) {
  FILE* f = std::fopen(path.c_str(), "wb");
  REQUIRE(f != nullptr);
  REQUIRE(std::fwrite(content.data(), 1, content.size(), f) == content.size());
  std::fclose(f);
}

// Constructs the cache and returns its error text (empty when valid) — the
// one-liner every constructor-side error arc asserts on.
std::string probeError(const std::string& cache_dir, const std::string& url) {
  soar::HttpCache cache(cache_dir, url);
  return cache.valid() ? std::string() : cache.error();
}

// Serializes a meta file exactly like saveMeta does, so individual fields
// can be corrupted before planting it (one loadMeta reject arc per variant).
std::string craftMeta(const std::string& url, uint64_t size, uint32_t blocks,
                      const std::string& bitmap, uint32_t version = 1,
                      int url_len_override = -1) {
  std::string m("SOARCHN1", 8);
  auto put32 = [&m](uint32_t v) {
    for (int i = 0; i < 4; ++i) m.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
  };
  auto put64 = [&m](uint64_t v) {
    for (int i = 0; i < 8; ++i) m.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
  };
  put32(version);
  put32(url_len_override >= 0 ? static_cast<uint32_t>(url_len_override)
                              : static_cast<uint32_t>(url.size()));
  m += url;
  put64(size);
  put32(blocks);
  m += bitmap;
  return m;
}

uint32_t blocksFor(uint64_t size) {
  return static_cast<uint32_t>((size + kBlockSize - 1) / kBlockSize);
}

// Parses a live meta file back (the read-side mirror of craftMeta) so tests
// can pin exactly which blocks the cache believes it holds — the ground
// truth behind "a seek fills just its own hole" style claims.
struct MetaBitmap {
  uint64_t size = 0;
  uint32_t blocks = 0;
  std::vector<bool> cached;
};

MetaBitmap readMetaBitmap(const std::string& cache_dir, const std::string& url) {
  FILE* f = std::fopen((cache_dir + "/" + cacheStem(url) + ".meta").c_str(), "rb");
  REQUIRE(f != nullptr);
  std::string m;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) m.append(buf, n);
  std::fclose(f);

  auto get32 = [&m](size_t off) {
    return static_cast<uint32_t>(static_cast<uint8_t>(m[off])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(m[off + 1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(m[off + 2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(m[off + 3])) << 24);
  };
  auto get64 = [&](size_t off) {
    return static_cast<uint64_t>(get32(off)) |
           (static_cast<uint64_t>(get32(off + 4)) << 32);
  };

  MetaBitmap out;
  REQUIRE(m.size() >= 8 + 4 + 4);
  CHECK(m.compare(0, 8, "SOARCHN1") == 0);
  CHECK(get32(8) == 1);  // version
  const uint32_t url_len = get32(12);
  size_t off = 16;
  REQUIRE(m.size() >= off + url_len + 8 + 4);
  CHECK(m.compare(off, url_len, url) == 0);
  off += url_len;
  out.size = get64(off);
  off += 8;
  out.blocks = get32(off);
  off += 4;
  REQUIRE(out.blocks == blocksFor(out.size));
  const size_t bitmap_bytes = (out.blocks + 7) / 8;
  REQUIRE(m.size() == off + bitmap_bytes);
  // Bit b lives at 0x80 >> (b % 8) inside byte b / 8 — MSB first, matching
  // the product's bitmap_[block >> 3] |= 0x80u >> (block & 7).
  out.cached.assign(out.blocks, false);
  for (uint32_t b = 0; b < out.blocks; ++b) {
    out.cached[b] = ((static_cast<uint8_t>(m[off + b / 8]) >> (7 - (b % 8))) & 1) != 0;
  }
  return out;
}

// Grabs an ephemeral TCP port (bind :0, read it back, close) so "connection
// refused" cases never depend on low ports being refused instantly — some
// runner firewalls turn a closed low port into a silent drop, which would
// cost a full connect timeout per use. The tiny release-to-bind race is
// negligible: the port comes from the ephemeral range, far from every
// fixture server's fixed range.
int freeTcpPort() {
  const int s = ::socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) return -1;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(s);
    return -1;
  }
  socklen_t slen = sizeof(addr);
  if (::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &slen) != 0) {
    ::close(s);
    return -1;
  }
  ::close(s);
  // No :: prefix: on BSD-derived systems ntohs is a function-like macro,
  // and "::ntohs(...)" is a syntax error. (Linux tolerates both.)
  return ntohs(addr.sin_port);
}

// RAII guard around RLIMIT_FSIZE: every destructor path restores the
// original limits, so a REQUIRE failure mid-case cannot leak a crippled
// limit into the sibling cases of the same binary.
struct FsizeLimit {
  rlimit old{};
  bool lowered = false;
  explicit FsizeLimit(rlim_t soft) {
    rlimit lim{};
    if (::getrlimit(RLIMIT_FSIZE, &old) != 0) return;
    lim = old;
    lim.rlim_cur = soft;
    lowered = ::setrlimit(RLIMIT_FSIZE, &lim) == 0;
  }
  ~FsizeLimit() {
    if (lowered) ::setrlimit(RLIMIT_FSIZE, &old);
  }
};

#endif  // !_WIN32

}  // namespace

TEST_CASE("https and garbage urls are rejected without touching the network") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  soar::HttpCache https("/tmp/soar_cache_reject_unused", "https://example.com/media.bin");
  CHECK(!https.valid());
  CHECK(https.error().find("http://") != std::string::npos);

  soar::HttpCache ftp("/tmp/soar_cache_reject_unused", "ftp://example.com/media.bin");
  CHECK(!ftp.valid());
#endif
}

TEST_CASE("a full read over a range server returns the exact source bytes") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping http cache tests");
    return;
  }
  const std::string dir = makeTempDir("full");
  REQUIRE(!dir.empty());
  const std::string src = makeSourceFile(dir, "full.bin", 600000);
  REQUIRE(!src.empty());
  RangeServer srv = startRangeServer(dir, 18600);
  REQUIRE(srv.pid >= 0);

  {
    soar::HttpCache cache(dir + "/cache", srv.base_url + "/full.bin");
    REQUIRE(cache.valid());
    CHECK(cache.size() == 600000);
    CHECK(cache.cachedBytes() == 0);

    std::vector<uint8_t> buf(600000);
    REQUIRE(cache.read(0, buf.data(), buf.size()) == buf.size());
    CHECK(std::memcmp(buf.data(), readSourceSlice(src, 0, 600000).data(), 600000) == 0);
    CHECK(cache.cachedBytes() == 600000);
    CHECK(cache.read(600000, buf.data(), 10) == 0);  // EOF
  }

  srv.stop();
  removeTree(dir);
#endif
}

TEST_CASE("out-of-order partial reads fill blocks across block boundaries") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping http cache tests");
    return;
  }
  const std::string dir = makeTempDir("holes");
  REQUIRE(!dir.empty());
  const std::string src = makeSourceFile(dir, "holes.bin", 700000);  // 2.7 blocks
  REQUIRE(!src.empty());
  RangeServer srv = startRangeServer(dir, 18640);
  REQUIRE(srv.pid >= 0);

  {
    soar::HttpCache cache(dir + "/cache", srv.base_url + "/holes.bin");
    REQUIRE(cache.valid());

    // First block only.
    std::vector<uint8_t> buf(100);
    REQUIRE(cache.read(0, buf.data(), buf.size()) == buf.size());
    CHECK(std::memcmp(buf.data(), readSourceSlice(src, 0, 100).data(), 100) == 0);
    CHECK(cache.cachedBytes() == kBlockSize);

    // Cross the block 0 -> 1 boundary.
    const size_t boundary = kBlockSize - 4;
    REQUIRE(cache.read(boundary, buf.data(), buf.size()) == buf.size());
    CHECK(std::memcmp(buf.data(), readSourceSlice(src, boundary, 100).data(), 100) == 0);

    // Tail (inside the last partial block).
    REQUIRE(cache.read(699000, buf.data(), buf.size()) == buf.size());
    CHECK(std::memcmp(buf.data(), readSourceSlice(src, 699000, 100).data(), 100) == 0);

    // Middle span over blocks 1 and 2.
    std::vector<uint8_t> mid(50000);
    REQUIRE(cache.read(300000, mid.data(), mid.size()) == mid.size());
    CHECK(std::memcmp(mid.data(), readSourceSlice(src, 300000, 50000).data(), 50000) == 0);

    // Every block was touched; the whole file must now be cached.
    CHECK(cache.cachedBytes() == 700000);
    CHECK(cache.read(700000, buf.data(), 1) == 0);
  }

  srv.stop();
  removeTree(dir);
#endif
}

TEST_CASE("cached blocks survive a reopen and resume instead of refetching") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping http cache tests");
    return;
  }
  const std::string dir = makeTempDir("resume");
  REQUIRE(!dir.empty());
  const std::string src = makeSourceFile(dir, "resume.bin", 700000);
  REQUIRE(!src.empty());
  RangeServer srv = startRangeServer(dir, 18680);
  REQUIRE(srv.pid >= 0);
  const std::string url = srv.base_url + "/resume.bin";

  {
    soar::HttpCache first(dir + "/cache", url);
    REQUIRE(first.valid());
    std::vector<uint8_t> buf(1000);
    REQUIRE(first.read(0, buf.data(), buf.size()) == buf.size());
  }
  {
    soar::HttpCache second(dir + "/cache", url);
    REQUIRE(second.valid());
    CHECK(second.cachedBytes() == kBlockSize);  // restored from the meta
    CHECK(second.fetchAll());
    CHECK(second.cachedBytes() == second.size());

    std::vector<uint8_t> buf(700000);
    REQUIRE(second.read(0, buf.data(), buf.size()) == buf.size());
    CHECK(std::memcmp(buf.data(), readSourceSlice(src, 0, 700000).data(), 700000) == 0);
  }

  srv.stop();
  removeTree(dir);
#endif
}

TEST_CASE("a meta file naming a different url is rebuilt, not trusted") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping http cache tests");
    return;
  }
  const std::string dir = makeTempDir("meta");
  REQUIRE(!dir.empty());
  const std::string src = makeSourceFile(dir, "meta.bin", 600000);
  REQUIRE(!src.empty());
  RangeServer srv = startRangeServer(dir, 18720);
  REQUIRE(srv.pid >= 0);

  const std::string url_a = srv.base_url + "/meta.bin";
  // url_b must be a real, live source with the same size: the rebuild
  // path under test requires a successful size probe; only the meta
  // handshake (which belongs to url_a) must be rejected.
  const std::string url_b = srv.base_url + "/other-name.bin";
  REQUIRE(!makeSourceFile(dir, "other-name.bin", 600000).empty());
  const std::string stem_a = cacheStem(url_a);
  const std::string stem_b = cacheStem(url_b);
  // Cache under url_a, then clone its files onto url_b's names: url_b's
  // meta now claims to belong to url_a and must not be trusted.
  {
    soar::HttpCache a(dir + "/cache", url_a);
    REQUIRE(a.valid());
    std::vector<uint8_t> buf(1000);
    REQUIRE(a.read(0, buf.data(), buf.size()) == buf.size());
    REQUIRE(::rename((a.metaPath()).c_str(),
                     (dir + "/cache/" + stem_b + ".meta").c_str()) == 0);
    REQUIRE(::rename((a.dataPath()).c_str(),
                     (dir + "/cache/" + stem_b + ".data").c_str()) == 0);
  }

  {
    soar::HttpCache b(dir + "/cache", url_b);
    REQUIRE(b.valid());
    CHECK(b.cachedBytes() == 0);  // bitmap was rebuilt, nothing trusted

    std::vector<uint8_t> buf(600000);
    REQUIRE(b.read(0, buf.data(), buf.size()) == buf.size());
    CHECK(std::memcmp(buf.data(), readSourceSlice(src, 0, 600000).data(), 600000) == 0);
  }

  srv.stop();
  removeTree(dir);
#endif
}

TEST_CASE("a server without range support is rejected with a clear error") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping http cache tests");
    return;
  }
  const std::string dir = makeTempDir("norange");
  REQUIRE(!dir.empty());
  REQUIRE(!makeSourceFile(dir, "plain.bin", 4096).empty());

  // Plain server: answers 200 with the whole file, no Range support.
  const std::string port_str = std::to_string(18760 + (::getpid() % 200));
  char* const argv[] = {
    const_cast<char*>("python3"),
    const_cast<char*>("-c"),
    const_cast<char*>(kPlainServerScript),
    const_cast<char*>(dir.c_str()),
    const_cast<char*>(port_str.c_str()),
    nullptr,
  };
  const pid_t pid = startPipedServer(argv);
  if (pid >= 0) {
    soar::HttpCache cache(dir + "/cache",
                          "http://127.0.0.1:" + port_str + "/plain.bin");
    CHECK(!cache.valid());
    CHECK(cache.error().find("byte ranges") != std::string::npos);
  } else {
    MESSAGE("plain HTTP server failed to start; skipping no-range test");
  }

  if (pid >= 0) {
    ::kill(pid, SIGTERM);
  }
  ::waitpid(pid, nullptr, 0);
  removeTree(dir);
#endif
}

// Port bases for the new servers: 24000+, clear of every other suite's
// range (the media suite sits at 15000, the CLI suite at 15000/18200-18500
// plus a 18000+pid%2000 lottery that cannot reach up here). All new tests
// below are POSIX-only like the existing ones.

TEST_CASE("malformed authorities fail the parse before any network traffic") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  const std::string dir = makeTempDir("parse");
  REQUIRE(!dir.empty());
  for (const char* authority :
       {"/path",             // empty authority
        ":80/x",             // empty host
        "127.0.0.1:/x",      // empty port
        "127.0.0.1:notaport/x",  // non-numeric port
        "[::1",              // unclosed bracket
        "[::1]x"}) {         // junk between bracket and port
    CAPTURE(authority);
    const std::string err = probeError(dir + "/c", std::string("http://") + authority);
    CHECK(err.find("not a usable http:// url") != std::string::npos);
  }

  // An empty cache dir is rejected before anything is touched, and an
  // invalid instance refuses reads with its own message.
  CHECK(probeError("", "http://127.0.0.1:1/x.bin").find("cache dir is empty") !=
        std::string::npos);
  soar::HttpCache bad(dir + "/c", "ftp://host/file.bin");
  REQUIRE(!bad.valid());
  uint8_t byte = 0;
  CHECK(bad.read(0, &byte, 1) == soar::HttpCache::npos);
  CHECK(bad.error() == "http cache: instance is not valid");
  removeTree(dir);
#endif
}

TEST_CASE("resolve and connect failures are distinguishable") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  const std::string dir = makeTempDir("resolve");
  REQUIRE(!dir.empty());
  // .invalid is guaranteed never to resolve (RFC 2606); the url also has no
  // path, exercising the authority-only parse arm. Fake-ip DNS proxies
  // resolve it anyway (198.18.0.0/15), so ask the local resolver first and
  // skip when interception makes the premise false — CI has honest DNS.
  {
    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    addrinfo* result = nullptr;
    const int rc = ::getaddrinfo("soar-nonexistent-host.invalid", "80", &hints, &result);
    if (rc == 0) ::freeaddrinfo(result);
    if (rc == 0) {
      MESSAGE(".invalid resolves here (DNS interception); skipping the "
              "cannot-resolve sub-case");
    } else {
      const std::string err =
          probeError(dir + "/c", "http://soar-nonexistent-host.invalid");
      CHECK(err.find("cannot resolve") != std::string::npos);
    }
  }

  // A just-released ephemeral port is never listening: a refusal must read
  // as "cannot connect", not as a resolver failure.
  const int port = freeTcpPort();
  if (port > 0) {
    const std::string refused =
        probeError(dir + "/c2", "http://127.0.0.1:" + std::to_string(port) + "/x.bin");
    CHECK(refused.find("cannot connect to 127.0.0.1:" + std::to_string(port)) !=
          std::string::npos);
  }
  removeTree(dir);
#endif
}

TEST_CASE("a bracketed IPv6 literal url connects and serves exact bytes") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping ipv6 test");
    return;
  }
  const std::string dir = makeTempDir("v6");
  REQUIRE(!dir.empty());
  const std::string src = makeSourceFile(dir, "v6.bin", 300000);
  REQUIRE(!src.empty());
  RangeServer srv = startRangeServerV6(dir, 24560);
  if (srv.pid < 0) {
    MESSAGE("no IPv6 loopback on this host; skipping the ipv6 test");
    removeTree(dir);
    return;
  }
  const std::string url = srv.base_url + "/v6.bin";  // http://[::1]:port/v6.bin
  {
    soar::HttpCache cache(dir + "/cache", url);
    REQUIRE(cache.valid());
    std::vector<uint8_t> buf(300000);
    REQUIRE(cache.read(0, buf.data(), buf.size()) == buf.size());
    CHECK(std::memcmp(buf.data(), readSourceSlice(src, 0, 300000).data(), 300000) == 0);
  }

  // No port after the bracket: parse still accepts (port defaults to 80)
  // and the failure must be a *connect* refusal against the bare "::1" —
  // proving the brackets were stripped for getaddrinfo but kept in the
  // Host header form. macOS runners are slow to refuse a connect to
  // port 80 (firewall policy turns the RST into a drop), so the arc is
  // only exercised where it is deterministic; see coverage-notes §3.7.
#ifdef __APPLE__
  MESSAGE("macOS: skipping the default-port-80 connect arc (runner firewalls drop instead of refuse)");
#else
  const std::string err = probeError(dir + "/cache_noport", "http://[::1]/x.bin");
  CHECK(err.find("cannot connect to [::1]") != std::string::npos);
#endif

  srv.stop();
  removeTree(dir);
#endif
}

TEST_CASE("broken header phase: early close, flood, garbage, mid-body cut") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping raw server tests");
    return;
  }
  const std::string dir = makeTempDir("rawhdr");
  REQUIRE(!dir.empty());
  struct Mode {
    const char* mode;
    int port;
    const char* needle;
  };
  const std::vector<Mode> modes = {
      {"close_early", 24000, "connection closed before response headers"},
      {"bigheaders", 24040, "response headers exceed 64 KiB"},
      {"garbage", 24080, "malformed status line"},
      {"truncated", 24120, "connection closed mid-body (0/1"},
  };
  for (const auto& m : modes) {
    RangeServer srv = startRawServer(m.mode, m.port, 1000);
    REQUIRE(srv.pid >= 0);
    CAPTURE(m.mode);
    const std::string err = probeError(dir + "/c_" + m.mode, srv.base_url + "/x.bin");
    CHECK(err.find(m.needle) != std::string::npos);
    srv.stop();
  }
  removeTree(dir);
#endif
}

TEST_CASE("redirects, chunked encoding and odd statuses are rejected") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping raw server tests");
    return;
  }
  const std::string dir = makeTempDir("status");
  REQUIRE(!dir.empty());
  struct Mode {
    const char* mode;
    int port;
    const char* needle;
  };
  const std::vector<Mode> modes = {
      {"redirect", 24160, "redirects are not supported (status 302)"},
      {"chunked", 24200, "chunked transfer encoding is not supported"},
      {"notfound", 24240, "unexpected status 404"},
      // 1xx informational answers are not a final response either.
      {"status100", 24880, "unexpected status 100"},
      // 206 without a Content-Range is not a usable range answer either:
      // the probe must say so instead of reporting a network failure.
      {"probe206norange", 24280, "server does not support byte ranges (status 206)"},
  };
  for (const auto& m : modes) {
    RangeServer srv = startRawServer(m.mode, m.port, 600000);
    REQUIRE(srv.pid >= 0);
    CAPTURE(m.mode);
    const std::string err = probeError(dir + "/c_" + m.mode, srv.base_url + "/x.bin");
    CHECK(err.find(m.needle) != std::string::npos);
    srv.stop();
  }
  removeTree(dir);
#endif
}

TEST_CASE("a giant request to a peer that never reads fails the send") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping send-failure test");
    return;
  }
  // send() on a reset connection raises SIGPIPE; the component must be
  // testable with the default disposition left aside, so ignore it here.
  ::signal(SIGPIPE, SIG_IGN);
  const std::string dir = makeTempDir("sendfail");
  REQUIRE(!dir.empty());
  RangeServer srv = startRawServer("close_early", 24520, 0);
  REQUIRE(srv.pid >= 0);
  // The 8 MiB query cannot fit any socket buffer, so sendAll runs into the
  // peer's reset instead of finishing the write.
  const std::string url = srv.base_url + "/pad.bin?" + std::string(8u << 20, 'a');
  const std::string err = probeError(dir + "/c", url);
  CHECK(err.find("send failed (errno ") != std::string::npos);
  srv.stop();
  removeTree(dir);
#endif
}

TEST_CASE("range-fetch contract violations each produce their own error") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping fetch violation tests");
    return;
  }
  const std::string dir = makeTempDir("fetchbad");
  REQUIRE(!dir.empty());
  // Each mode answers the bytes=0-0 probe correctly (so the cache opens
  // online with size 600000) and sabotages the block fetch that follows.
  struct Mode {
    const char* mode;
    int port;
    const char* needle;
  };
  const std::vector<Mode> modes = {
      {"fetch200", 24320,
       "expected 206 Partial Content for a range fetch (status 200)"},
      {"fetchnorange", 24480,
       "expected 206 Partial Content for a range fetch (status 206)"},
      {"fetchwrongrange", 24360, "server returned a different range than requested"},
      {"fetchwrongtotal", 24400, "source size changed (cache 600000, server 600001)"},
      {"fetchshort", 24440, "short range body (131072 of 262144 bytes)"},
      {"fetchmalrange", 24840,
       "expected 206 Partial Content for a range fetch (status 206)"},
  };
  for (const auto& m : modes) {
    RangeServer srv = startRawServer(m.mode, m.port, 600000);
    REQUIRE(srv.pid >= 0);
    CAPTURE(m.mode);
    soar::HttpCache cache(dir + "/c_" + m.mode, srv.base_url + "/x.bin");
    REQUIRE(cache.valid());  // the probe was answered properly
    std::vector<uint8_t> buf(64);
    CHECK(cache.read(0, buf.data(), buf.size()) == soar::HttpCache::npos);
    CHECK(cache.error().find(m.needle) != std::string::npos);
    if (std::string(m.mode) == "fetchwrongrange") {
      // A mid-file read mismatches on range_first (echo says 0-0), the
      // block-0 retry above mismatches on range_last — both arms of the
      // same check must trip.
      CHECK(cache.read(300000, buf.data(), buf.size()) == soar::HttpCache::npos);
      CHECK(cache.error().find(m.needle) != std::string::npos);
    }
    if (std::string(m.mode) == "fetchmalrange") {
      // The server cycles through all five malformed Content-Range forms
      // (all-whitespace, wrong unit, unparseable numbers, both wrong
      // separators); every one of them must end in the has_range=false
      // arm instead of a trusted parse.
      for (int i = 0; i < 4; ++i) {
        CHECK(cache.read(0, buf.data(), buf.size()) == soar::HttpCache::npos);
        CHECK(cache.error().find(m.needle) != std::string::npos);
      }
    }
    srv.stop();
  }
  removeTree(dir);
#endif
}

TEST_CASE("a data file truncated behind the bitmap fails reads cleanly") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping vanish test");
    return;
  }
  const std::string dir = makeTempDir("vanish");
  REQUIRE(!dir.empty());
  const std::string src = makeSourceFile(dir, "vanish.bin", 600000);
  REQUIRE(!src.empty());
  RangeServer srv = startRangeServer(dir, 24600);
  REQUIRE(srv.pid >= 0);
  const std::string url = srv.base_url + "/vanish.bin";
  {
    soar::HttpCache cache(dir + "/cache", url);
    REQUIRE(cache.valid());
    REQUIRE(cache.fetchAll());
    CHECK(cache.cachedBytes() == 600000);
  }
  srv.stop();
  {
    // Offline reopen: the meta is the only source of truth now.
    soar::HttpCache cache(dir + "/cache", url);
    REQUIRE(cache.valid());
    CHECK(cache.cachedBytes() == 600000);
    // Sabotage: the data file goes away behind the bitmap's back. The read
    // must report a positional-read failure instead of returning garbage.
    REQUIRE(::truncate(cache.dataPath().c_str(), 0) == 0);
    std::vector<uint8_t> buf(100);
    CHECK(cache.read(0, buf.data(), buf.size()) == soar::HttpCache::npos);
    CHECK(cache.error().find("read from data file failed") != std::string::npos);
  }
  removeTree(dir);
#endif
}

TEST_CASE("a meta path occupied by a directory fails meta persistence and saves") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping occupied-meta test");
    return;
  }
  const std::string dir = makeTempDir("occupied");
  REQUIRE(!dir.empty());
  const std::string src = makeSourceFile(dir, "occupied.bin", 600000);
  REQUIRE(!src.empty());
  RangeServer srv = startRangeServer(dir, 24640);
  REQUIRE(srv.pid >= 0);
  const std::string url = srv.base_url + "/occupied.bin";
  const std::string cache_dir = dir + "/cache";
  REQUIRE(::mkdir(cache_dir.c_str(), 0755) == 0);
  // rename(tmp, target) cannot replace a directory with a file: every meta
  // save fails at the finalize step while the cache itself stays usable
  // in memory — and block fetches must refuse to pretend otherwise. The
  // keeper file matters: remove() would happily rmdir an empty directory
  // out of the way and the rename would then succeed.
  REQUIRE(::mkdir((cache_dir + "/" + cacheStem(url) + ".meta").c_str(), 0755) == 0);
  writeBytes(cache_dir + "/" + cacheStem(url) + ".meta/keep", "occupied");
  {
    soar::HttpCache cache(cache_dir, url);
    CHECK(cache.valid());
    CHECK(cache.error().find("cannot finalize meta file") != std::string::npos);
    std::vector<uint8_t> buf(64);
    CHECK(cache.read(0, buf.data(), buf.size()) == soar::HttpCache::npos);
    CHECK(cache.error().find("cannot finalize meta file") != std::string::npos);
  }
  srv.stop();
  removeTree(dir);
#endif
}

TEST_CASE("a regular file where the cache dir belongs fails the data open") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping blocker test");
    return;
  }
  const std::string dir = makeTempDir("blocker");
  REQUIRE(!dir.empty());
  REQUIRE(!makeSourceFile(dir, "blocker.bin", 4096).empty());
  RangeServer srv = startRangeServer(dir, 24680);
  REQUIRE(srv.pid >= 0);
  const std::string url = srv.base_url + "/blocker.bin";
  const std::string blocker = dir + "/blocker";
  writeBytes(blocker, "in the way");
  const std::string err = probeError(blocker + "/cache", url);
  CHECK(err.find("cannot open data file") != std::string::npos);
  CHECK(err.find("Not a directory") != std::string::npos);
  srv.stop();
  removeTree(dir);
#endif
}

TEST_CASE("a file size quota turns cache growth into clean errors") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping quota test");
    return;
  }
  // SIGXFSZ rides along with RLIMIT_FSIZE violations; the default
  // disposition would kill the binary mid-case.
  ::signal(SIGXFSZ, SIG_IGN);
  const std::string dir = makeTempDir("quota");
  REQUIRE(!dir.empty());
  const std::string src = makeSourceFile(dir, "quota.bin", 600000);
  REQUIRE(!src.empty());
  RangeServer srv = startRangeServer(dir, 24720);
  REQUIRE(srv.pid >= 0);
  const std::string url = srv.base_url + "/quota.bin";
  const std::string cache_dir = dir + "/cache";
  {
    // Fill exactly one block so the meta trusts block 0 only.
    soar::HttpCache first(cache_dir, url);
    REQUIRE(first.valid());
    std::vector<uint8_t> buf(100);
    REQUIRE(first.read(0, buf.data(), buf.size()) == buf.size());
  }

  {
    // Writes that would grow the data file past the quota fail at the
    // positional write, after the (allowed) range fetch. The cache is
    // constructed BEFORE the quota drops: BSD-derived systems (macOS)
    // limit-check ftruncate against the new size even when the file
    // already has exactly that size, so an online open under the lowered
    // limit would fail before the write arc is ever reached.
    soar::HttpCache online(cache_dir, url);
    REQUIRE(online.valid());
    FsizeLimit limit(1024);
    REQUIRE(limit.lowered);
    REQUIRE(::truncate(online.dataPath().c_str(), 1000) == 0);
    std::vector<uint8_t> buf(64);
    if (online.read(300000, buf.data(), buf.size()) == soar::HttpCache::npos) {
      CHECK(online.error().find("write to data file failed") != std::string::npos);
      CHECK(online.error().find("File too large") != std::string::npos);
    } else {
      MESSAGE("RLIMIT_FSIZE is not enforced for pwrite on this platform; "
              "the write-failure arc stays unverified here");
    }
  }

  srv.stop();
  {
    // With the data file gone, the constructor's own grow-to-size step is
    // what fails now — the cache refuses to open rather than lying about
    // an empty file holding 600000 bytes.
    FsizeLimit limit(1024);
    REQUIRE(limit.lowered);
    REQUIRE(::unlink((cache_dir + "/" + cacheStem(url) + ".data").c_str()) == 0);
    soar::HttpCache offline(cache_dir, url);
    CHECK(!offline.valid());
    CHECK(offline.error().find("cannot size data file to 600000") != std::string::npos);
    CHECK(offline.error().find("File too large") != std::string::npos);
  }
  removeTree(dir);
#endif
}

TEST_CASE("an oversized meta file is refused instead of loaded") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  const std::string dir = makeTempDir("bigmeta");
  REQUIRE(!dir.empty());
  const int dead_port = freeTcpPort();
  REQUIRE(dead_port > 0);
  const std::string url = "http://127.0.0.1:" + std::to_string(dead_port) + "/oversize.bin";
  const std::string cache_dir = dir + "/cache";
  REQUIRE(::mkdir(cache_dir.c_str(), 0755) == 0);
  // 5 MiB of junk: readWholeFile must bail at the 4 MiB cap instead of
  // feeding the parser a truncated monster.
  writeBytes(cache_dir + "/" + cacheStem(url) + ".meta", std::string(5u << 20, 'j'));
  const std::string err = probeError(cache_dir, url);
  CHECK(err.find("source unreachable") != std::string::npos);
  removeTree(dir);
#endif
}

TEST_CASE("every tampered meta variant is rebuilt, not trusted") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping tampered-meta test");
    return;
  }
  const std::string dir = makeTempDir("tampered");
  REQUIRE(!dir.empty());
  REQUIRE(!makeSourceFile(dir, "tampered.bin", 600000).empty());
  RangeServer srv = startRangeServer(dir, 24760);
  REQUIRE(srv.pid >= 0);
  const std::string url = srv.base_url + "/tampered.bin";
  const std::string cache_dir = dir + "/cache";
  REQUIRE(::mkdir(cache_dir.c_str(), 0755) == 0);
  const std::string meta_path = cache_dir + "/" + cacheStem(url) + ".meta";
  const std::string bitmap1(1, '\x80');  // block 0 set

  // Each variant corrupts exactly one loadMeta checkpoint; every one of
  // them must end in a rebuild (bitmap zeroed), never in a trusted load.
  std::string bad_magic = craftMeta(url, 600000, 3, bitmap1);
  bad_magic[3] = 'X';
  std::string tampered_url = url;
  tampered_url[tampered_url.size() - 5] = 'g';  // "tampered.bin" -> "tamperg.bin"
  const std::string header_only = craftMeta(url, 600000, 3, bitmap1).substr(0, 16);
  std::string tail_missing = craftMeta(url, 600000, 3, bitmap1);
  tail_missing.resize(tail_missing.size() - 5);  // url_len fine, file too short

  struct Variant {
    const char* name;
    std::string meta;
  };
  const std::vector<Variant> variants = {
      {"header truncated mid-magic", craftMeta(url, 600000, 3, bitmap1).substr(0, 12)},
      {"magic byte flipped", bad_magic},
      {"version bumped", craftMeta(url, 600000, 3, bitmap1, /*version=*/2)},
      {"url_len too small", craftMeta(url, 600000, 3, bitmap1, 1,
                                      static_cast<int>(url.size()) - 1)},
      {"url_len right but file short", tail_missing},
      {"different url, same length", craftMeta(tampered_url, 600000, 3, bitmap1)},
      {"header only, no payload", header_only},
      {"size mismatch", craftMeta(url, 600001, 3, bitmap1)},
      {"block count mismatch", craftMeta(url, 600000, 2, bitmap1)},
      {"bitmap missing", craftMeta(url, 600000, 3, "")},
      {"bitmap too long", craftMeta(url, 600000, 3, std::string(2, '\0'))},
  };
  for (const auto& v : variants) {
    CAPTURE(v.name);
    writeBytes(meta_path, v.meta);
    soar::HttpCache cache(cache_dir, url);
    CHECK(cache.valid());  // probed size wins, meta is rebuilt
    CHECK(cache.cachedBytes() == 0);
  }
  srv.stop();
  removeTree(dir);
#endif
}

TEST_CASE("fetchAll fails cleanly when the source dies before the warm-up") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping warm-up test");
    return;
  }
  const std::string dir = makeTempDir("warmup");
  REQUIRE(!dir.empty());
  REQUIRE(!makeSourceFile(dir, "warmup.bin", 600000).empty());
  RangeServer srv = startRangeServer(dir, 24800);
  REQUIRE(srv.pid >= 0);
  const std::string url = srv.base_url + "/warmup.bin";
  soar::HttpCache cache(dir + "/cache", url);
  REQUIRE(cache.valid());
  srv.stop();
  CHECK_FALSE(cache.fetchAll());
  CHECK(cache.error().find("cannot connect") != std::string::npos);
  removeTree(dir);
#endif
}

#if defined(SOAR_WITH_FFMPEG) && !defined(_WIN32)

#include "soar/core/ffmpeg_backend.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace std::chrono_literals;

namespace {

struct StateSink : soar::IEventSink {
  std::atomic<int> ended{0};
  std::atomic<int> errors{0};
  // DownloadProgress capture (P3c): the decode thread emits synchronously,
  // so the series itself is single-threaded; the atomics exist for the
  // polling test thread.
  std::atomic<int> progress{0};
  std::atomic<std::uint64_t> last_downloaded{0};
  std::atomic<std::uint64_t> progress_total{0};
  std::atomic<bool> progress_monotonic{true};
  void onEvent(const soar::Event& e) override {
    if (e.type == soar::EventType::StateChanged &&
        e.state == soar::PlaybackState::Ended) {
      ++ended;
    } else if (e.type == soar::EventType::Error) {
      ++errors;
    } else if (e.type == soar::EventType::DownloadProgress) {
      // Payload rides in message as "downloaded/total" (Event must not
      // grow fields — backend.h / coverage-notes §3.8).
      unsigned long long have = 0, total = 0;
      if (std::sscanf(e.message.c_str(), "%llu/%llu", &have, &total) == 2) {
        if (have < last_downloaded.load(std::memory_order_relaxed)) {
          progress_monotonic.store(false, std::memory_order_relaxed);
        }
        last_downloaded.store(have, std::memory_order_relaxed);
        progress_total.store(total, std::memory_order_relaxed);
        ++progress;
      }
    }
  }
};

}  // namespace

TEST_CASE("playback through the cache replays offline after the server dies") {
  // The integration promise of P3a: play an http:// source once with
  // --cache-dir semantics, kill the server, reopen the same url + dir —
  // the second open takes the offline-meta path and playback advances
  // purely from disk.
  std::string media = std::getenv("SOAR_TEST_AUDIO_ONLY") == nullptr
                          ? ""
                          : std::getenv("SOAR_TEST_AUDIO_ONLY");
  if (media.empty()) {
    MESSAGE("SOAR_TEST_AUDIO_ONLY not set; skipping cache replay test");
    return;
  }
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping cache replay test");
    return;
  }

  const std::string dir = makeTempDir("replay");
  REQUIRE(!dir.empty());
  // Serve a copy: the fixture path itself must never double as a live
  // server root, and the server needs the file under its own root for the
  // probe to answer 206.
  const std::string name = media.substr(media.find_last_of('/') + 1);
  REQUIRE(std::system(("cp '" + media + "' '" + dir + "/" + name + "'").c_str()) == 0);
  RangeServer srv = startRangeServer(dir, 18800);
  REQUIRE(srv.pid >= 0);
  const std::string url = srv.base_url + "/" + name;
  const std::string cache_dir = dir + "/cache";

  // First pass: online playback, filling the cache. Poll the clock instead
  // of betting on the Ended event — headless machines without an audio
  // device may never emit it (the media suite's playback cases use the
  // same position-based contract; never bet on wall clocks).
  {
    StateSink sink;
    auto backend = soar::makeFFmpegBackend();
    backend->setEventSink(&sink);
    REQUIRE(backend->open(soar::MediaSource{url, cache_dir}));
    REQUIRE(backend->setRate(8.0));
    REQUIRE(backend->play());

    const auto deadline = std::chrono::steady_clock::now() + 240s;
    while (std::chrono::steady_clock::now() < deadline &&
           backend->position() < std::chrono::milliseconds(3000) &&
           sink.ended.load() == 0) {
      std::this_thread::sleep_for(50ms);
    }
    const bool advanced = backend->position() >= std::chrono::milliseconds(3000) ||
                          sink.ended.load() >= 1;
    backend->stop();
    backend->close();
    CHECK(advanced);
    CHECK(sink.errors.load() == 0);
  }

  // The cache must hold the whole source now.
  uint64_t cached_bytes = 0;
  {
    soar::HttpCache probe(cache_dir, url);
    REQUIRE(probe.valid());
    CHECK(probe.fetchAll());
    cached_bytes = probe.cachedBytes();
    CHECK(cached_bytes == probe.size());
  }

  // Kill the server: from here on there is no network at all.
  srv.stop();

  // Second pass: offline reopen + playback from disk only.
  {
    StateSink sink;
    auto backend = soar::makeFFmpegBackend();
    backend->setEventSink(&sink);
    REQUIRE(backend->open(soar::MediaSource{url, cache_dir}));
    REQUIRE(backend->setRate(8.0));
    REQUIRE(backend->play());

    bool advanced = false;
    const auto deadline = std::chrono::steady_clock::now() + 240s;
    while (std::chrono::steady_clock::now() < deadline) {
      if (backend->position() > std::chrono::milliseconds(0) || sink.ended.load() > 0) {
        advanced = true;
        break;
      }
      std::this_thread::sleep_for(50ms);
    }
    CHECK(advanced);
    backend->stop();
    backend->close();
    CHECK(sink.errors.load() == 0);
  }

  removeTree(dir);
}

TEST_CASE("seeking into an uncached region fills just that hole and the partial cache replays offline") {
  // P3b's integration promise, first half: a mid-playback seek to a far,
  // never-fetched region issues a Range fetch for THAT region only — the
  // bitmap gains the seek-target block(s) while the span between the
  // header and the target stays empty. Second half: kill the server, and
  // the partial (hole-ridden) cache still replays the cached span offline.
  std::string media = std::getenv("SOAR_TEST_MEDIA") == nullptr
                          ? ""
                          : std::getenv("SOAR_TEST_MEDIA");
  if (media.empty()) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping seek hole test");
    return;
  }
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping seek hole test");
    return;
  }

  const std::string dir = makeTempDir("seekhole");
  REQUIRE(!dir.empty());
  const std::string name = media.substr(media.find_last_of('/') + 1);
  REQUIRE(std::system(("cp '" + media + "' '" + dir + "/" + name + "'").c_str()) == 0);
  RangeServer srv = startRangeServer(dir, 24920);
  REQUIRE(srv.pid >= 0);
  const std::string url = srv.base_url + "/" + name;
  const std::string cache_dir = dir + "/cache";

  // Pass 1 (online): open, seek to ~58% of the duration while stopped (the
  // synchronous state-machine path), then play past it — far enough to walk
  // off the seek target's blocks into the next hole, so the cache also
  // grows *while the decode loop is running* (that growth is what P3c's
  // progress series reports; a stopped-state seek fetches outside the loop
  // and stays silent by design). The sink is shared with pass 2 so the
  // offline pass can be pinned as progress-silent.
  int64_t dur_ms = 0;
  StateSink sink;
  {
    auto backend = soar::makeFFmpegBackend();
    backend->setEventSink(&sink);
    REQUIRE(backend->open(soar::MediaSource{url, cache_dir}));
    dur_ms = backend->mediaInfo().duration.count();
    REQUIRE(dur_ms >= 5000);
    const auto target = std::chrono::milliseconds(3500);
    // Well short of the end (the offline pass below must have a hole left
    // to miss) but past the seek target's blocks.
    const auto play_to = std::chrono::milliseconds(dur_ms) * 5 / 6;
    CHECK(backend->mediaInfo().seekable);
    CHECK(backend->seek(target));
    CHECK(backend->position() == target);

    REQUIRE(backend->setRate(8.0));
    REQUIRE(backend->play());
    const auto deadline = std::chrono::steady_clock::now() + 240s;
    while (std::chrono::steady_clock::now() < deadline &&
           backend->position() < play_to && sink.ended.load() == 0) {
      std::this_thread::sleep_for(50ms);
    }
    CHECK(backend->position() >= play_to);
    backend->stop();
    backend->close();
    CHECK(sink.errors.load() == 0);
  }
  // P3c online contract: growing the cache emits throttled progress.
  CHECK(sink.progress.load() >= 1);
  CHECK(sink.progress_monotonic.load());
  const int progress_after_online = sink.progress.load();

  // The bitmap must show the seek-target block cached and the file as a
  // whole NOT fully cached: 3500ms of this ~6s source lands past the file
  // midpoint, and the pass stops at 5/6, so the blocks between header and
  // target stay empty.
  const MetaBitmap after_seek = readMetaBitmap(cache_dir, url);
  const size_t target_block = static_cast<size_t>(
      static_cast<uint64_t>(after_seek.size) * 3500 /
      static_cast<uint64_t>(dur_ms) / kBlockSize);
  CHECK(after_seek.cached[target_block]);
  size_t cached_count = 0;
  for (bool b : after_seek.cached) {
    if (b) ++cached_count;
  }
  CHECK(cached_count < after_seek.blocks);
  // The progress events named the true source size.
  CHECK(sink.progress_total.load() == after_seek.size);

  // Offline probe: the meta alone (no server) already knows the partial
  // state.
  uint64_t offline_cached_bytes = 0;
  {
    soar::HttpCache probe(cache_dir, url);
    REQUIRE(probe.valid());
    offline_cached_bytes = probe.cachedBytes();
    CHECK(offline_cached_bytes < probe.size());
  }

  // Kill the server: no network from here on.
  srv.stop();

  // Pass 2 (offline resume): reopen on the partial cache and play the
  // cached span — target .. 4000ms sits inside the blocks pass 1 filled.
  {
    auto backend = soar::makeFFmpegBackend();
    backend->setEventSink(&sink);
    REQUIRE(backend->open(soar::MediaSource{url, cache_dir}));
    CHECK(backend->seek(std::chrono::milliseconds(3500)));
    REQUIRE(backend->setRate(8.0));
    REQUIRE(backend->play());
    const auto deadline = std::chrono::steady_clock::now() + 240s;
    while (std::chrono::steady_clock::now() < deadline &&
           backend->position() < std::chrono::milliseconds(4000) &&
           sink.ended.load() == 0) {
      std::this_thread::sleep_for(50ms);
    }
    CHECK(backend->position() >= std::chrono::milliseconds(4000));
    backend->stop();
    backend->close();
    CHECK(sink.errors.load() == 0);
  }
  // P3c offline contract: a fully-served session emits no new progress —
  // the throttle step never moves because cachedBytes never grows.
  CHECK(sink.progress.load() == progress_after_online);

  // Offline playback must not have "grown" the cache.
  {
    soar::HttpCache probe(cache_dir, url);
    REQUIRE(probe.valid());
    CHECK(probe.cachedBytes() == offline_cached_bytes);
  }

  removeTree(dir);
}

TEST_CASE("an offline read inside a hole surfaces as an Error event, not a hang") {
  // P3b's failure contract: a partial cache with head and tail blocks but a
  // hole in the middle replays fine until playback walks into the hole —
  // then the avio adapter turns the cache miss into EIO and the decode
  // loop reports an Error instead of stalling forever.
  std::string media = std::getenv("SOAR_TEST_MEDIA") == nullptr
                          ? ""
                          : std::getenv("SOAR_TEST_MEDIA");
  if (media.empty()) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping offline hole test");
    return;
  }
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping offline hole test");
    return;
  }

  const std::string dir = makeTempDir("holeerr");
  REQUIRE(!dir.empty());
  const std::string name = media.substr(media.find_last_of('/') + 1);
  REQUIRE(std::system(("cp '" + media + "' '" + dir + "/" + name + "'").c_str()) == 0);
  RangeServer srv = startRangeServer(dir, 24960);
  REQUIRE(srv.pid >= 0);
  const std::string url = srv.base_url + "/" + name;
  const std::string cache_dir = dir + "/cache";

  // Component-level partial fill: block 0 (header) and the last block
  // (tail, where matroska keeps its Cues) — everything between is a hole.
  {
    soar::HttpCache cache(cache_dir, url);
    REQUIRE(cache.valid());
    std::vector<uint8_t> buf(kBlockSize);
    CHECK(cache.read(0, buf.data(), buf.size()) == kBlockSize);
    // Offset of the last block (works for exact multiples too).
    const uint64_t tail = ((cache.size() - 1) / kBlockSize) * kBlockSize;
    CHECK(cache.read(tail, buf.data(),
                     static_cast<size_t>(cache.size() - tail)) ==
          cache.size() - tail);
  }
  const MetaBitmap partial = readMetaBitmap(cache_dir, url);
  CHECK(partial.cached[0]);
  CHECK(partial.cached[partial.blocks - 1]);
  size_t cached_count = 0;
  for (bool b : partial.cached) {
    if (b) ++cached_count;
  }
  CHECK(cached_count == 2);

  srv.stop();

  // Component-level: an offline read inside the hole misses cleanly.
  {
    soar::HttpCache probe(cache_dir, url);
    REQUIRE(probe.valid());
    uint8_t byte = 0;
    CHECK(probe.read(kBlockSize, &byte, 1) == soar::HttpCache::npos);
    CHECK_FALSE(probe.error().empty());
  }

  // Backend-level: playback consumes block 0 and then hits the hole.
  {
    StateSink sink;
    auto backend = soar::makeFFmpegBackend();
    backend->setEventSink(&sink);
    REQUIRE(backend->open(soar::MediaSource{url, cache_dir}));
    REQUIRE(backend->setRate(8.0));
    REQUIRE(backend->play());
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    while (std::chrono::steady_clock::now() < deadline &&
           sink.errors.load() == 0 && sink.ended.load() == 0) {
      std::this_thread::sleep_for(50ms);
    }
    CHECK(sink.errors.load() >= 1);
    backend->stop();
    backend->close();
  }

  removeTree(dir);
}

TEST_CASE("playing a cached http source to its end ends playback instead of stalling") {
  // The avio EOF contract, pinned without a window. Reading past the last
  // cached byte must return AVERROR_EOF: the read callback used to return
  // 0-as-success, and FFmpeg then treated it as "no data yet" and retried
  // forever, so a cached source that reached its tail spun the decode
  // thread instead of ending (found by the P3c windowed download test).
  // Component level here so the regression does not need X11 to be caught.
  std::string media = std::getenv("SOAR_TEST_MEDIA") == nullptr
                          ? ""
                          : std::getenv("SOAR_TEST_MEDIA");
  if (media.empty()) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping cached EOF test");
    return;
  }
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping cached EOF test");
    return;
  }

  const std::string dir = makeTempDir("cacheeof");
  REQUIRE(!dir.empty());
  const std::string name = media.substr(media.find_last_of('/') + 1);
  REQUIRE(std::system(("cp '" + media + "' '" + dir + "/" + name + "'").c_str()) == 0);
  RangeServer srv = startRangeServer(dir, 25000);
  REQUIRE(srv.pid >= 0);
  const std::string url = srv.base_url + "/" + name;
  const std::string cache_dir = dir + "/cache";

  StateSink sink;
  int64_t dur_ms = 0;
  {
    auto backend = soar::makeFFmpegBackend();
    backend->setEventSink(&sink);
    REQUIRE(backend->open(soar::MediaSource{url, cache_dir}));
    dur_ms = backend->mediaInfo().duration.count();
    REQUIRE(dur_ms > 0);
    REQUIRE(backend->setRate(8.0));
    REQUIRE(backend->play());

    // Ended, or an Error, or the deadline: the point is that *one of the
    // two* arrives — a stuck decode loop is the regression.
    const auto deadline = std::chrono::steady_clock::now() + 120s;
    while (std::chrono::steady_clock::now() < deadline &&
           sink.ended.load() == 0 && sink.errors.load() == 0) {
      std::this_thread::sleep_for(50ms);
    }
    CHECK(sink.ended.load() == 1);
    CHECK(sink.errors.load() == 0);
    // Playback ran to the tail rather than ending early.
    CHECK(backend->position() > std::chrono::milliseconds(dur_ms) / 2);
    backend->stop();
    backend->close();
  }

  // Sequential playback caches the whole source, so the progress series
  // terminates at downloaded == total — the exact condition the download
  // chip hides on.
  const MetaBitmap full = readMetaBitmap(cache_dir, url);
  for (size_t i = 0; i < full.cached.size(); ++i) {
    CHECK(full.cached[i]);
  }
  CHECK(sink.progress.load() >= 1);
  CHECK(sink.progress_monotonic.load());
  CHECK(sink.progress_total.load() == full.size);
  CHECK(sink.last_downloaded.load() == full.size);

  removeTree(dir);
}

#endif  // SOAR_WITH_FFMPEG && !_WIN32
