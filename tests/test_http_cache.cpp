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
#include <cstring>
#include <random>
#include <string>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using test_servers::kPlainServerScript;
using test_servers::kRangeServerScript;
using test_servers::RangeServer;
using test_servers::startRangeServer;
using test_servers::startPipedServer;
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

#if defined(SOAR_WITH_FFMPEG) && !defined(_WIN32)

#include "soar/core/ffmpeg_backend.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;

namespace {

struct StateSink : soar::IEventSink {
  std::atomic<int> ended{0};
  std::atomic<int> errors{0};
  void onEvent(const soar::Event& e) override {
    if (e.type == soar::EventType::StateChanged &&
        e.state == soar::PlaybackState::Ended) {
      ++ended;
    } else if (e.type == soar::EventType::Error) {
      ++errors;
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

#endif  // SOAR_WITH_FFMPEG && !_WIN32
