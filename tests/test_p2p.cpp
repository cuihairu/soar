// Tests for the P2P streaming bridge (src/p2p, docs/mvp.md §5 P4):
// TorrentStream's start/stop contract, its failure surface, and the local
// HTTP server the player opens as "http://127.0.0.1:<port>/" (Range
// semantics, method handling, multi-file indexing).
//
// Every case is self-contained: payload bytes go into a fresh temp store,
// a v1 .torrent is built over them with the same libtorrent API the
// seed_torrent dev tool uses, and TorrentStream serves its own files — the
// local hash check marks every piece present after add, so no swarm,
// tracker or second process is involved. POSIX sockets only: like the
// cache suite, Windows builds report a skip and return.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "torrent_stream.h"

#include <libtorrent/bencode.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/file_storage.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace lt = libtorrent;
namespace p2p = soar::p2p;

namespace {

// A 40-hex btih nobody can fetch: the metadata wait never ends inside a
// test, so it is only ever used where stop() or a synchronous failure is
// what gets exercised.
constexpr const char* kDeadMagnet =
    "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567";

#ifndef _WIN32

std::string makeTempDir(const char* tag) {
  std::string tmpl = std::string("/tmp/soar_p2p_") + tag + "_XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  const char* dir = ::mkdtemp(buf.data());
  REQUIRE(dir != nullptr);
  return std::string(dir);
}

// Deterministic payload: byte i is a pure function of i, so any sliced
// range can be verified against the same formula without keeping the
// whole string around.
std::string patternBytes(size_t n) {
  std::string s;
  s.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    s.push_back(static_cast<char>((i * 31 + i % 7) & 0xff));
  }
  return s;
}

void writeBytes(const std::string& path, const std::string& bytes) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  REQUIRE(f.good());
  f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  REQUIRE(f.good());
}

bool fileExists(const std::string& path) {
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0;
}

// Builds a v1 torrent over `rel_paths` (relative to `root`, in the order
// given — directory enumeration order is OS-dependent and nothing to
// assert against). Payload files must already exist under root with the
// sizes declared here; root doubles as TorrentStream's store_dir, so the
// payload is pre-seeded pieces: the hash check right after add finds
// every piece on disk. Writes the .torrent to `out`.
std::string buildTorrent(const std::string& root,
                         const std::vector<std::string>& rel_paths,
                         const std::string& out) {
  REQUIRE(!rel_paths.empty());
  lt::file_storage fs;
  for (const std::string& rel : rel_paths) {
    struct stat st {};
    const std::string full = root + "/" + rel;
    REQUIRE(::stat(full.c_str(), &st) == 0);
    fs.add_file(rel, static_cast<std::int64_t>(st.st_size));
  }
  // piece size 0 lets create_torrent pick by size; v1_only keeps the
  // layout deterministic across libtorrent versions.
  lt::create_torrent ct(fs, 0, lt::create_torrent::v1_only);
  ct.set_creator("soar-test_p2p/1");
  lt::set_piece_hashes(ct, root, [](lt::piece_index_t) {});

  std::vector<char> buf;
  lt::bencode(std::back_inserter(buf), ct.generate());
  std::ofstream f(out, std::ios::binary | std::ios::trunc);
  REQUIRE(f.good());
  f.write(buf.data(), static_cast<std::streamsize>(buf.size()));
  REQUIRE(f.good());
  return out;
}

// One HTTP request/response exchange on a connection to the playback
// bridge. Reads are bounded (SO_RCVTIMEO) so a server-side stall surfaces
// as a failed exchange instead of a wedged test process.
struct Reply {
  int status = 0;
  std::string status_line;
  // Lower-cased header names; values verbatim.
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;

  const std::string* header(const std::string& name) const {
    for (const auto& kv : headers) {
      if (kv.first == name) return &kv.second;
    }
    return nullptr;
  }
};

class Conn {
 public:
  ~Conn() { close(); }

  bool connectTo(const std::string& playback_url) {
    // "http://127.0.0.1:<port>/" — the only shape playbackUrl() returns.
    const size_t host_at = playback_url.find("//");
    REQUIRE(host_at != std::string::npos);
    const size_t port_at = playback_url.find(':', host_at + 2);
    REQUIRE(port_at != std::string::npos);
    const int port = std::atoi(playback_url.c_str() + port_at + 1);
    REQUIRE(port > 0);

    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd_ >= 0);
    timeval tv{};
    tv.tv_sec = 70;  // above kPieceWaitMs, below a stuck ctest window
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<unsigned short>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
  }

  void close() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  bool send(const std::string& text) const {
    size_t off = 0;
    while (off < text.size()) {
      const ssize_t w = ::send(fd_, text.data() + off, text.size() - off, 0);
      if (w <= 0) return false;
      off += static_cast<size_t>(w);
    }
    return true;
  }

  // Reads one reply. `expect_body` follows the request method: a HEAD
  // carries Content-Length but no body, while 4xx replies here have no
  // length at all and end with the server closing the connection.
  bool readReply(Reply* out, bool expect_body) {
    *out = Reply{};
    std::string head;
    size_t head_end;
    while ((head_end = head.find("\r\n\r\n")) == std::string::npos) {
      char tmp[4096];
      const ssize_t r = ::recv(fd_, tmp, sizeof(tmp), 0);
      if (r <= 0) return false;
      head.append(tmp, static_cast<size_t>(r));
    }
    // The recv that completed the head may have carried body bytes too —
    // keep them, the read below starts from there.
    std::string spill = head.substr(head_end + 4);
    head.resize(head_end);
    out->status_line = head.substr(0, head.find("\r\n"));
    if (out->status_line.size() < 12) return false;
    out->status = std::atoi(out->status_line.c_str() + 9);

    size_t pos = head.find("\r\n") + 2;
    while (pos < head_end) {
      const size_t eol = head.find("\r\n", pos);
      const size_t colon = head.find(':', pos);
      if (colon == std::string::npos || colon > head_end) break;
      std::string name = head.substr(pos, colon - pos);
      for (char& c : name) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      }
      size_t v = colon + 1;
      while (v < head_end && (head[v] == ' ' || head[v] == '\t')) ++v;
      const size_t value_end = eol == std::string::npos ? head_end : eol;
      out->headers.emplace_back(name, head.substr(v, value_end - v));
      if (eol == std::string::npos) break;
      pos = eol + 2;
    }
    if (!expect_body) return true;

    size_t content_length = 0;
    if (const std::string* cl = out->header("content-length")) {
      content_length = static_cast<size_t>(std::strtoull(cl->c_str(), nullptr, 10));
      out->body = spill;
      while (out->body.size() < content_length) {
        char tmp[8192];
        const ssize_t r = ::recv(fd_, tmp, sizeof(tmp), 0);
        if (r <= 0) return false;
        out->body.append(tmp, static_cast<size_t>(r));
      }
      return true;
    }
    // No length: the reply ends with the connection (405/416 here).
    out->body = spill;
    while (true) {
      char tmp[4096];
      const ssize_t r = ::recv(fd_, tmp, sizeof(tmp), 0);
      if (r <= 0) break;
      out->body.append(tmp, static_cast<size_t>(r));
    }
    return true;
  }

  bool exchange(const std::string& request, Reply* out, bool expect_body = true) {
    if (!send(request)) return false;
    return readReply(out, expect_body);
  }

 private:
  int fd_ = -1;
};

std::string get(const std::string& path = "/") {
  return "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: keep-alive\r\n\r\n";
}

// Waits until the startAsync worker publishes `want` (or the deadline
// passes). A .torrent reaches Serving without any network, so seconds is
// generous even under coverage instrumentation.
bool waitPhase(const p2p::TorrentStream& stream, p2p::TorrentPhase want,
               int timeout_ms = 15000) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (stream.phase() != want) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return true;
}

// A one-file torrent in a fresh store: writes `bytes` as media.bin, builds
// the .torrent next to it, returns both paths. The store root is the
// directory itself so the payload doubles as pre-seeded pieces.
struct SoloStore {
  std::string dir;
  std::string torrent;
  std::string payload_path;
  std::string bytes;
};

SoloStore makeSolo(const char* tag, size_t payload_size) {
  SoloStore s;
  s.dir = makeTempDir(tag);
  s.bytes = patternBytes(payload_size);
  s.payload_path = s.dir + "/media.bin";
  writeBytes(s.payload_path, s.bytes);
  s.torrent = buildTorrent(s.dir, {"media.bin"}, s.dir + "/media.torrent");
  return s;
}

#endif  // !_WIN32

}  // namespace

#ifdef _WIN32

TEST_CASE("p2p suite is POSIX-only") {
  MESSAGE("Windows: torrent_stream's socket layer is POSIX-socket based "
          "here; the suite reports a skip (same shape as the cache tests)");
}

#else

TEST_CASE("start validates its params before touching the network") {
  p2p::TorrentStream stream;

  p2p::TorrentStream::Params empty;
  CHECK_FALSE(stream.start(empty));
  CHECK(stream.lastError().find("no .torrent path or magnet URI given") !=
        std::string::npos);
  CHECK(stream.phase() == p2p::TorrentPhase::Idle);

  p2p::TorrentStream::Params no_store;
  no_store.torrent_path = "/nonexistent/anything.torrent";
  CHECK_FALSE(stream.startAsync(no_store));
  CHECK(stream.lastError().find("no store dir given") != std::string::npos);

  // Validation rejects both entry points identically, and a failed start
  // leaves the object reusable (impl_ never took hold).
  CHECK_FALSE(stream.start(no_store));
  CHECK(stream.phase() == p2p::TorrentPhase::Idle);
}

TEST_CASE("an unreadable or corrupt .torrent fails fast") {
  p2p::TorrentStream stream;
  p2p::TorrentStream::Params p;
  p.torrent_path = "/nonexistent/soar-nope.torrent";
  p.store_dir = "/tmp/soar_p2p_unused_store";
  CHECK_FALSE(stream.start(p));
  CHECK(stream.lastError().find("torrent: cannot parse /nonexistent/soar-nope.torrent") !=
        std::string::npos);

  const std::string dir = makeTempDir("corrupt");
  const std::string junk = dir + "/junk.torrent";
  writeBytes(junk, "this is not bencoded metadata at all");
  p.torrent_path = junk;
  CHECK_FALSE(stream.start(p));
  CHECK(stream.lastError().find("torrent: cannot parse " + junk) != std::string::npos);
}

TEST_CASE("a malformed magnet fails at parse") {
  p2p::TorrentStream stream;
  p2p::TorrentStream::Params p;
  p.magnet_uri = "magnet:?xt=urn:btih:zzzz";
  p.store_dir = "/tmp/soar_p2p_unused_store";
  CHECK_FALSE(stream.start(p));
  CHECK(stream.lastError().find("torrent: cannot parse magnet URI") != std::string::npos);
}

TEST_CASE("malformed peer endpoints are rejected before listening") {
  p2p::TorrentStream stream;
  p2p::TorrentStream::Params p;
  p.magnet_uri = kDeadMagnet;
  p.store_dir = "/tmp/soar_p2p_unused_store";

  // Each shape trips a different guard in the peer loop: missing colon,
  // empty host, empty port, port out of range, unresolvable host. The
  // listen socket is only created after this loop, so the same object can
  // retry — a failed prepare never takes hold as the impl.
  p.peers = {":80"};
  CHECK_FALSE(stream.start(p));
  CHECK(stream.lastError().find("bad peer (want host:port)") != std::string::npos);

  p.peers = {"127.0.0.1:"};
  CHECK_FALSE(stream.start(p));
  CHECK(stream.lastError().find("bad peer (want host:port)") != std::string::npos);

  p.peers = {"127.0.0.1:0"};
  CHECK_FALSE(stream.start(p));
  CHECK(stream.lastError().find("bad peer port in") != std::string::npos);

  p.peers = {"127.0.0.1:70000"};
  CHECK_FALSE(stream.start(p));
  CHECK(stream.lastError().find("bad peer port in") != std::string::npos);

  p.peers = {"no-such-host.invalid:80"};
  CHECK_FALSE(stream.start(p));
  CHECK(stream.lastError().find("bad peer no-such-host.invalid:") != std::string::npos);
}

TEST_CASE("a self-seeded torrent serves the whole file over local HTTP") {
  const SoloStore s = makeSolo("solo", 120 * 1024);
  p2p::TorrentStream stream;

  int progress_calls = 0;
  p2p::TorrentStatus last_progress;
  std::mutex progress_mu;
  p2p::TorrentStream::Params p;
  p.torrent_path = s.torrent;
  p.store_dir = s.dir;
  p.on_progress = [&](const p2p::TorrentStatus& st) {
    std::lock_guard<std::mutex> lock(progress_mu);
    ++progress_calls;
    last_progress = st;
  };

  REQUIRE(stream.start(p));
  CHECK(stream.phase() == p2p::TorrentPhase::Serving);
  CHECK(stream.playbackUrl().rfind("http://127.0.0.1:", 0) == 0);
  CHECK(stream.fileSize() == s.bytes.size());
  CHECK(stream.fileName() == "media.bin");
  CHECK(stream.lastError().empty());

  Conn conn;
  REQUIRE(conn.connectTo(stream.playbackUrl()));
  Reply rep;
  REQUIRE(conn.exchange(get(), &rep));
  CHECK(rep.status == 200);
  CHECK(rep.body == s.bytes);
  const std::string* cl = rep.header("content-length");
  REQUIRE(cl != nullptr);
  CHECK(*cl == std::to_string(s.bytes.size()));
  REQUIRE(rep.header("accept-ranges") != nullptr);
  CHECK(*rep.header("accept-ranges") == "bytes");

  // The monitor thread refreshes status() ~2 Hz and rate-limits the
  // callback to ~1 Hz; by now it has ticked at least once with the
  // .torrent metadata present and the local check complete.
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  const p2p::TorrentStatus st = stream.status();
  CHECK(st.metadata);
  CHECK(st.total == s.bytes.size());
  CHECK(st.downloaded == s.bytes.size());
  std::lock_guard<std::mutex> lock(progress_mu);
  CHECK(progress_calls >= 1);
  CHECK(last_progress.metadata);

  stream.stop();
  CHECK(stream.phase() == p2p::TorrentPhase::Idle);
  // stop() is idempotent and status() degrades to zeros once torn down.
  stream.stop();
  const p2p::TorrentStatus idle = stream.status();
  CHECK(idle.total == 0);
  CHECK(idle.peers == 0);
}

TEST_CASE("range requests answer 206 with exact bytes in all three forms") {
  const SoloStore s = makeSolo("range", 90 * 1024);
  p2p::TorrentStream stream;
  p2p::TorrentStream::Params p;
  p.torrent_path = s.torrent;
  p.store_dir = s.dir;
  REQUIRE(stream.start(p));

  Conn conn;
  REQUIRE(conn.connectTo(stream.playbackUrl()));

  // Closed range, header casing capitalized (the lookup is case-
  // insensitive over the raw head).
  Reply rep;
  REQUIRE(conn.exchange("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nRange: bytes=100-199\r\n"
                        "Connection: keep-alive\r\n\r\n",
                        &rep));
  CHECK(rep.status == 206);
  REQUIRE(rep.header("content-range") != nullptr);
  CHECK(*rep.header("content-range") ==
        "bytes 100-199/" + std::to_string(s.bytes.size()));
  CHECK(rep.body == s.bytes.substr(100, 100));

  // Open-ended — libavformat's http demuxer default probe.
  REQUIRE(conn.exchange("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nrange: bytes=50000-\r\n"
                        "Connection: keep-alive\r\n\r\n",
                        &rep));
  CHECK(rep.status == 206);
  CHECK(*rep.header("content-range") ==
        "bytes 50000-" + std::to_string(s.bytes.size() - 1) + "/" +
            std::to_string(s.bytes.size()));
  CHECK(rep.body == s.bytes.substr(50000));

  // Suffix form — the other probe shape.
  REQUIRE(conn.exchange("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nrange: bytes=-512\r\n"
                        "Connection: keep-alive\r\n\r\n",
                        &rep));
  CHECK(rep.status == 206);
  CHECK(*rep.header("content-range") ==
        "bytes " + std::to_string(s.bytes.size() - 512) + "-" +
            std::to_string(s.bytes.size() - 1) + "/" + std::to_string(s.bytes.size()));
  CHECK(rep.body == s.bytes.substr(s.bytes.size() - 512));
}

TEST_CASE("unsatisfiable or malformed ranges answer 416") {
  const SoloStore s = makeSolo("badrange", 40 * 1024);
  p2p::TorrentStream stream;
  p2p::TorrentStream::Params p;
  p.torrent_path = s.torrent;
  p.store_dir = s.dir;
  REQUIRE(stream.start(p));

  // Each shape walks a different rejection arm of the parser: first past
  // EOF, non-digits on either side of the dash, a missing dash, a suffix
  // longer than the file, a zero suffix, and a non-bytes unit. The reply
  // carries the size and closes the connection, so every probe reconnects.
  const std::vector<std::string> bad_ranges = {
      "bytes=999999-",       // first >= size
      "bytes=abc-def",       // non-digit first
      "bytes=10-x",          // non-digit last
      "bytes=5",             // no dash at all
      "bytes=-999999",       // suffix >= size
      "bytes=-0",            // zero suffix
      "items=1-2",           // wrong unit
      "bytes=",              // nothing after the prefix
  };
  for (const std::string& range : bad_ranges) {
    Conn conn;
    REQUIRE(conn.connectTo(stream.playbackUrl()));
    Reply rep;
    const bool ok = conn.exchange(
        "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nrange: " + range +
            "\r\nConnection: keep-alive\r\n\r\n",
        &rep);
    REQUIRE(ok);
    CHECK(rep.status == 416);
    REQUIRE(rep.header("content-range") != nullptr);
    CHECK(*rep.header("content-range") == "bytes */" + std::to_string(s.bytes.size()));
  }
}

TEST_CASE("HEAD answers headers only and other methods are refused") {
  const SoloStore s = makeSolo("methods", 30 * 1024);
  p2p::TorrentStream stream;
  p2p::TorrentStream::Params p;
  p.torrent_path = s.torrent;
  p.store_dir = s.dir;
  REQUIRE(stream.start(p));

  Conn conn;
  REQUIRE(conn.connectTo(stream.playbackUrl()));

  // HEAD carries the full Content-Length but no body; the same connection
  // then serves a normal GET (keep-alive loop).
  Reply rep;
  REQUIRE(conn.exchange("HEAD / HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                        "Connection: keep-alive\r\n\r\n",
                        &rep, /*expect_body=*/false));
  CHECK(rep.status == 200);
  REQUIRE(rep.header("content-length") != nullptr);
  CHECK(*rep.header("content-length") == std::to_string(s.bytes.size()));
  REQUIRE(conn.exchange(get(), &rep));
  CHECK(rep.status == 200);
  CHECK(rep.body == s.bytes);

  // A non-GET/HEAD method is refused with Allow and the connection closes.
  Reply refused;
  REQUIRE(conn.exchange("POST / HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                        "Content-Length: 0\r\nConnection: close\r\n\r\n",
                        &refused));
  CHECK(refused.status == 405);
  REQUIRE(refused.header("allow") != nullptr);
  CHECK(*refused.header("allow") == "GET, HEAD");
}

TEST_CASE("multi-file torrents serve the indexed entry with its own bytes") {
  const std::string dir = makeTempDir("multi");
  const std::string payload_dir = dir + "/multi";
  REQUIRE(::mkdir(payload_dir.c_str(), 0755) == 0);
  const std::string a_bytes = patternBytes(64 * 1024);
  const std::string b_bytes = patternBytes(45 * 1024);
  writeBytes(payload_dir + "/a.bin", a_bytes);
  writeBytes(payload_dir + "/b.bin", b_bytes);
  const std::string torrent =
      buildTorrent(dir, {"multi/a.bin", "multi/b.bin"}, dir + "/multi.torrent");

  p2p::TorrentStream stream;
  std::vector<p2p::TorrentFile> files;
  p2p::TorrentStream::Params p;
  p.torrent_path = torrent;
  p.store_dir = dir;
  p.file_index = 1;
  p.on_files = [&](const std::vector<p2p::TorrentFile>& table) { files = table; };

  REQUIRE(stream.start(p));
  CHECK(stream.fileName() == "b.bin");
  CHECK(stream.fileSize() == b_bytes.size());

  // on_files fires before selection, on the calling thread — the full
  // table is the picker affordance the CLI prints.
  REQUIRE(files.size() == 2);
  CHECK(files[0].index == 0);
  CHECK(files[0].path == "multi/a.bin");
  CHECK(files[0].size == a_bytes.size());
  CHECK(files[1].path == "multi/b.bin");
  CHECK(files[1].size == b_bytes.size());

  // Entry after the first has a non-zero file_base: bytes must come from
  // that entry's own file at file-relative offsets (the walk-caught bug).
  Conn conn;
  REQUIRE(conn.connectTo(stream.playbackUrl()));
  Reply rep;
  REQUIRE(conn.exchange(get(), &rep));
  CHECK(rep.status == 200);
  CHECK(rep.body == b_bytes);

  stream.stop();
}

TEST_CASE("an out-of-range file index reports the picker table") {
  const std::string dir = makeTempDir("idx");
  const std::string payload_dir = dir + "/idx";
  REQUIRE(::mkdir(payload_dir.c_str(), 0755) == 0);
  writeBytes(payload_dir + "/a.bin", patternBytes(1024));
  writeBytes(payload_dir + "/b.bin", patternBytes(2048));
  const std::string torrent =
      buildTorrent(dir, {"idx/a.bin", "idx/b.bin"}, dir + "/idx.torrent");

  // Without on_files the error carries the table itself, bounded at 32
  // entries so a pathological torrent cannot produce endless output.
  {
    p2p::TorrentStream stream;
    p2p::TorrentStream::Params p;
    p.torrent_path = torrent;
    p.store_dir = dir;
    p.file_index = 7;
    CHECK_FALSE(stream.start(p));
    CHECK(stream.lastError().find("file index 7 out of range (torrent has 2 files)") !=
          std::string::npos);
    CHECK(stream.lastError().find("  [0] 1024  idx/a.bin") != std::string::npos);
    CHECK(stream.lastError().find("  [1] 2048  idx/b.bin") != std::string::npos);
    CHECK(stream.phase() == p2p::TorrentPhase::Idle);
  }

  // With on_files the table was already delivered — the error must not
  // print it a second time.
  {
    p2p::TorrentStream stream;
    std::vector<p2p::TorrentFile> files;
    p2p::TorrentStream::Params p;
    p.torrent_path = torrent;
    p.store_dir = dir;
    p.file_index = 7;
    p.on_files = [&](const std::vector<p2p::TorrentFile>& table) { files = table; };
    CHECK_FALSE(stream.start(p));
    CHECK(files.size() == 2);
    CHECK(stream.lastError().find("file index 7 out of range") != std::string::npos);
    CHECK(stream.lastError().find("[0]") == std::string::npos);
  }
}

TEST_CASE("an empty payload file is refused before serving") {
  // A single 0-byte file cannot even be built into a torrent (libtorrent
  // rejects zero pieces outright), but a zero-size entry alongside a real
  // one loads fine — selection then trips the product's own guard.
  const std::string dir = makeTempDir("empty");
  const std::string payload_dir = dir + "/e";
  REQUIRE(::mkdir(payload_dir.c_str(), 0755) == 0);
  writeBytes(payload_dir + "/empty.bin", "");
  writeBytes(payload_dir + "/real.bin", patternBytes(1024));
  const std::string torrent =
      buildTorrent(dir, {"e/empty.bin", "e/real.bin"}, dir + "/e.torrent");

  p2p::TorrentStream stream;
  p2p::TorrentStream::Params p;
  p.torrent_path = torrent;
  p.store_dir = dir;
  CHECK_FALSE(stream.start(p));
  CHECK(stream.lastError().find("torrent: file is empty") != std::string::npos);
}

TEST_CASE("start is single-flight and stop returns the stream to idle") {
  const SoloStore s = makeSolo("lifecycle", 16 * 1024);
  p2p::TorrentStream stream;
  p2p::TorrentStream::Params p;
  p.torrent_path = s.torrent;
  p.store_dir = s.dir;

  REQUIRE(stream.start(p));
  CHECK_FALSE(stream.start(p));
  CHECK(stream.lastError().find("already started") != std::string::npos);
  // startAsync shares the same guard — a prepared stream never gets a
  // second session.
  CHECK_FALSE(stream.startAsync(p));
  CHECK(stream.lastError().find("already started") != std::string::npos);
  CHECK(stream.phase() == p2p::TorrentPhase::Serving);

  stream.stop();
  CHECK(stream.phase() == p2p::TorrentPhase::Idle);
  // stop() on an already-stopped stream is a no-op, and the object can
  // start over: a fresh prepare, a fresh listen socket.
  stream.stop();
  REQUIRE(stream.start(p));
  Conn conn;
  REQUIRE(conn.connectTo(stream.playbackUrl()));
  Reply rep;
  REQUIRE(conn.exchange(get(), &rep));
  CHECK(rep.status == 200);
  CHECK(rep.body == s.bytes);
  stream.stop();
}

TEST_CASE("startAsync publishes Serving and stop cancels a pending start") {
  const SoloStore s = makeSolo("async", 32 * 1024);
  p2p::TorrentStream stream;
  p2p::TorrentStream::Params p;
  p.torrent_path = s.torrent;
  p.store_dir = s.dir;

  REQUIRE(stream.startAsync(p));
  // The synchronous half finalizes the URL before returning; the worker
  // only has the (instant) metadata no-op left.
  CHECK(stream.playbackUrl().rfind("http://127.0.0.1:", 0) == 0);
  REQUIRE(waitPhase(stream, p2p::TorrentPhase::Serving));
  CHECK(stream.fileName() == "media.bin");

  stream.stop();
  CHECK(stream.phase() == p2p::TorrentPhase::Idle);

  // A magnet start parks in Connecting until the swarm delivers the
  // metadata (up to 60 s); stop() from the caller must interrupt that
  // wait instead of sitting it out — the phase settles on Idle, never
  // Failed, because stop() owns the final store. While it parks, the
  // metadata loop is the only view the caller has: it ticks on_progress
  // about twice a second (peers seen, no metadata yet). The explicit
  // peer endpoint also exercises the success half of the peer loop — a
  // well-formed host:port is contacted before the wait begins.
  p2p::TorrentStream magnet;
  int magnet_ticks = 0;
  bool tick_had_metadata = true;
  p2p::TorrentStream::Params mp;
  mp.magnet_uri = kDeadMagnet;
  mp.store_dir = s.dir;
  mp.peers = {"127.0.0.1:1"};
  mp.on_progress = [&](const p2p::TorrentStatus& st) {
    ++magnet_ticks;
    tick_had_metadata = st.metadata;
  };
  REQUIRE(magnet.startAsync(mp));
  CHECK(magnet.phase() == p2p::TorrentPhase::Connecting);
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  CHECK(magnet_ticks >= 1);
  CHECK_FALSE(tick_had_metadata);
  const auto began = std::chrono::steady_clock::now();
  magnet.stop();
  const auto waited = std::chrono::steady_clock::now() - began;
  CHECK(std::chrono::duration_cast<std::chrono::seconds>(waited).count() < 10);
  CHECK(magnet.phase() == p2p::TorrentPhase::Idle);
}

TEST_CASE("startAsync surfaces a tail failure as Failed") {
  const std::string dir = makeTempDir("failed");
  const std::string payload_dir = dir + "/f";
  REQUIRE(::mkdir(payload_dir.c_str(), 0755) == 0);
  writeBytes(payload_dir + "/a.bin", patternBytes(1024));
  writeBytes(payload_dir + "/b.bin", patternBytes(2048));
  const std::string torrent = buildTorrent(dir, {"f/a.bin", "f/b.bin"}, dir + "/f.torrent");

  p2p::TorrentStream stream;
  p2p::TorrentStream::Params p;
  p.torrent_path = torrent;
  p.store_dir = dir;
  p.file_index = 9;
  REQUIRE(stream.startAsync(p));
  REQUIRE(waitPhase(stream, p2p::TorrentPhase::Failed));
  CHECK(stream.lastError().find("file index 9 out of range") != std::string::npos);
  stream.stop();
  CHECK(stream.phase() == p2p::TorrentPhase::Idle);
}

TEST_CASE("a store file that vanishes after the check fails the read") {
  const SoloStore s = makeSolo("vanish", 48 * 1024);
  p2p::TorrentStream stream;
  p2p::TorrentStream::Params p;
  p.torrent_path = s.torrent;
  p.store_dir = s.dir;
  REQUIRE(stream.start(p));

  // Confirm the hash check finished and the file serves — only then
  // remove it. libtorrent keeps the in-memory have state, so the next
  // read passes waitForPiece immediately and pread hits a missing file.
  {
    Conn conn;
    REQUIRE(conn.connectTo(stream.playbackUrl()));
    Reply rep;
    REQUIRE(conn.exchange(get(), &rep));
    REQUIRE(rep.status == 200);
    REQUIRE(rep.body == s.bytes);
  }
  REQUIRE(::unlink(s.payload_path.c_str()) == 0);
  {
    Conn conn;
    REQUIRE(conn.connectTo(stream.playbackUrl()));
    Reply rep;
    // The server closes the connection without a response head.
    CHECK_FALSE(conn.exchange(get(), &rep));
  }
  stream.stop();

  // Same arc with a truncated file: the piece state still says "present",
  // but the short read makes preadFile refuse to hand back a partial
  // buffer.
  const SoloStore t = makeSolo("truncate", 48 * 1024);
  p2p::TorrentStream ts;
  p2p::TorrentStream::Params tp;
  tp.torrent_path = t.torrent;
  tp.store_dir = t.dir;
  REQUIRE(ts.start(tp));
  {
    Conn conn;
    REQUIRE(conn.connectTo(ts.playbackUrl()));
    Reply rep;
    REQUIRE(conn.exchange(get(), &rep));
    REQUIRE(rep.status == 200);
  }
  writeBytes(t.payload_path, t.bytes.substr(0, t.bytes.size() / 2));
  {
    Conn conn;
    REQUIRE(conn.connectTo(ts.playbackUrl()));
    Reply rep;
    CHECK_FALSE(conn.exchange(get(), &rep));
  }

  ts.stop();
}

TEST_CASE("an oversized or empty request head drops the connection") {
  const SoloStore s = makeSolo("badhead", 8 * 1024);
  p2p::TorrentStream stream;
  p2p::TorrentStream::Params p;
  p.torrent_path = s.torrent;
  p.store_dir = s.dir;
  REQUIRE(stream.start(p));

  // No CRLF CRLF in sight and past kMaxHeadBytes: the server gives up
  // without a response.
  {
    Conn conn;
    REQUIRE(conn.connectTo(stream.playbackUrl()));
    std::string junk = "GET / HTTP/1.1\r\nX-Junk: ";
    junk.append(40 * 1024, 'a');
    REQUIRE(conn.send(junk));
    Reply rep;
    CHECK_FALSE(conn.readReply(&rep, true));
  }

  // A client that connects and hangs up before sending anything just
  // costs one accept-loop iteration — the server keeps serving.
  {
    Conn conn;
    REQUIRE(conn.connectTo(stream.playbackUrl()));
    conn.close();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  Conn alive;
  REQUIRE(alive.connectTo(stream.playbackUrl()));
  Reply rep;
  REQUIRE(alive.exchange(get(), &rep));
  CHECK(rep.status == 200);
  CHECK(rep.body == s.bytes);

  stream.stop();
}

TEST_CASE("a client that hangs up mid-body fails the send quietly") {
  const SoloStore s = makeSolo("hangup", 4 * 1024 * 1024);
  p2p::TorrentStream stream;
  p2p::TorrentStream::Params p;
  p.torrent_path = s.torrent;
  p.store_dir = s.dir;
  REQUIRE(stream.start(p));

  {
    Conn conn;
    REQUIRE(conn.connectTo(stream.playbackUrl()));
    Reply rep;
    // Read just the head, then drop the socket: streamRange's send runs
    // into a closed peer (SIGPIPE is ignored at load time, so the send
    // fails instead of killing the process).
    REQUIRE(conn.send(get()));
    conn.close();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // The connection thread is disposable — the listener still accepts.
  Conn alive;
  REQUIRE(alive.connectTo(stream.playbackUrl()));
  Reply rep;
  REQUIRE(alive.exchange(get(), &rep));
  CHECK(rep.status == 200);
  CHECK(rep.body == s.bytes);

  stream.stop();
}

TEST_CASE("a request waiting on missing pieces gives up when the stream stops") {
  // Build the torrent, then remove the payload: the hash check finds
  // nothing on disk and nothing in the swarm can supply it, so the next
  // GET parks inside waitForPiece behind a piece deadline. stop() from
  // the caller must release that waiter — without it, teardown would
  // block behind the full 60 s piece wait.
  const SoloStore s = makeSolo("nopieces", 64 * 1024);
  REQUIRE(::unlink(s.payload_path.c_str()) == 0);
  p2p::TorrentStream stream;
  p2p::TorrentStream::Params p;
  p.torrent_path = s.torrent;
  p.store_dir = s.dir;
  REQUIRE(stream.start(p));

  std::thread client([&] {
    Conn conn;
    if (!conn.connectTo(stream.playbackUrl())) return;
    Reply rep;
    (void)conn.exchange(get(), &rep);  // outcome irrelevant: blocked or refused
  });
  // Long enough for the request to reach the handler and enter the piece
  // wait (loopback is microseconds; instrumentation only slows this).
  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  const auto began = std::chrono::steady_clock::now();
  stream.stop();
  const auto waited = std::chrono::steady_clock::now() - began;
  client.join();
  CHECK(std::chrono::duration_cast<std::chrono::seconds>(waited).count() < 10);
  CHECK(stream.phase() == p2p::TorrentPhase::Idle);
}

TEST_CASE("a magnet that never hears from the swarm times out explicitly") {
  // The full metadata wait (kMetadataWaitMs = 60 s): without a peer that
  // carries the info dictionary there is no honest answer other than this
  // timeout, with the peer count as the diagnostic. The most expensive
  // case in the suite by far — it exists because the timeout contract is
  // what a stuck magnet start actually promises.
  p2p::TorrentStream stream;
  p2p::TorrentStream::Params p;
  p.magnet_uri = kDeadMagnet;
  p.store_dir = makeTempDir("metawait");
  const auto began = std::chrono::steady_clock::now();
  CHECK_FALSE(stream.start(p));
  const auto waited = std::chrono::steady_clock::now() - began;
  CHECK(stream.lastError().find("torrent: timed out waiting for metadata") !=
        std::string::npos);
  CHECK(stream.lastError().find("peers seen:") != std::string::npos);
  CHECK(std::chrono::duration_cast<std::chrono::seconds>(waited).count() >= 55);
  CHECK(stream.phase() == p2p::TorrentPhase::Idle);
}

#endif  // !_WIN32
