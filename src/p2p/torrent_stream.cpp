#include "torrent_stream.h"

#include <libtorrent/address.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/load_torrent.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/socket.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

#ifdef _WIN32
#  include <fcntl.h>
#  include <io.h>
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <fcntl.h>
#  include <netinet/in.h>
#  include <signal.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace lt = libtorrent;

namespace soar::p2p {

namespace {

// Playback reads are served at this granularity: one wait cycle per chunk,
// so the sequential download front (or a piece deadline for a seek target)
// has to move at most this far to unblock the next read.
constexpr size_t kReadChunk = 256 * 1024;
// A piece the demuxer is waiting on gets this long to arrive before the
// handler drops the connection (FFmpeg surfaces that as an ordinary read
// error, which the backend turns into an Error event).
constexpr int kPieceWaitMs = 60000;
// Magnet-only: how long start() waits for the swarm to deliver the metadata
// (the info dictionary) before giving up with an explicit error. Peers come
// from the magnet's trackers, DHT bootstrap or explicit --torrent-peer
// endpoints; without any of them this timeout is the honest answer, not a
// hang.
constexpr int kMetadataWaitMs = 60000;
// Piece availability poll. libtorrent handles are thread-safe, so the wait
// loop needs no shared queue; every waiter just polls its own piece.
constexpr int kPollMs = 25;
constexpr size_t kMaxHeadBytes = 32 * 1024;
constexpr int kListenBacklog = 8;

#ifdef _WIN32
using socklen_t = int;

static void sockInit() {
  static const bool ready = [] {
    WSADATA d;
    return WSAStartup(MAKEWORD(2, 2), &d) == 0;
  }();
  (void)ready;
}
static void sockClose(int s) { closesocket(s); }
static int lastNetError() { return WSAGetLastError(); }
static int sockSend(int s, const char* p, int n) { return send(s, p, n, 0); }
static int sockRecv(int s, char* p, int n) { return recv(s, p, n, 0); }
static int sockAccept(int s) { return static_cast<int>(accept(s, nullptr, nullptr)); }
static int sockShutdown(int s) { return shutdown(s, SD_BOTH); }
static int fileClose(int fd) { return _close(fd); }
#else
// The server writes into sockets the player can drop at any moment (a seek
// aborts mid-body). A dead peer would otherwise raise SIGPIPE and kill the
// process instead of failing the send.
static const bool sigpipe_ignored = [] {
  ::signal(SIGPIPE, SIG_IGN);
  return true;
}();

static void sockInit() { (void)sigpipe_ignored; }
static void sockClose(int s) { ::close(s); }
static int lastNetError() { return errno; }
static int sockSend(int s, const char* p, int n) { return send(s, p, n, 0); }
static int sockRecv(int s, char* p, int n) { return recv(s, p, n, 0); }
static int sockAccept(int s) { return accept(s, nullptr, nullptr); }
static int sockShutdown(int s) { return shutdown(s, SHUT_RDWR); }
static int fileClose(int fd) { return ::close(fd); }
#endif

static bool sendAll(int fd, const char* p, size_t n) {
  while (n > 0) {
    const int w = sockSend(fd, p, static_cast<int>(std::min<size_t>(n, 1 << 20)));
    if (w <= 0) return false;
    p += w;
    n -= static_cast<size_t>(w);
  }
  return true;
}

static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Reads the portion of the served file at `off` straight from disk. The
// caller made sure every intersecting piece is complete, so the bytes on
// disk are final. Returns (size_t)-1 on failure.
size_t preadFile(const std::string& path, std::uint64_t off, std::uint8_t* dst, size_t n) {
#ifdef _WIN32
  const int fd = _open(path.c_str(), _O_RDONLY | _O_BINARY);
#else
  const int fd = ::open(path.c_str(), O_RDONLY);
#endif
  if (fd < 0) return static_cast<size_t>(-1);
  size_t done = 0;
  while (done < n) {
    const size_t want = std::min<size_t>(n - done, 1u << 30);
#ifdef _WIN32
    if (_lseeki64(fd, static_cast<__int64>(off), SEEK_SET) < 0) {
      fileClose(fd);
      return static_cast<size_t>(-1);
    }
    const int r = _read(fd, dst + done, static_cast<unsigned>(want));
#else
    const ssize_t r = pread(fd, dst + done, want, static_cast<off_t>(off));
#endif
    if (r <= 0) break;
    done += static_cast<size_t>(r);
    off += static_cast<std::uint64_t>(r);
  }
  fileClose(fd);
  return done == n ? n : static_cast<size_t>(-1);
}

// Everything a connection thread touches, so no call can reach a session
// the stream may have already torn down: the shared_ptr keeps libtorrent
// alive for as long as any connection might still use it, and the stop
// flag outlives the Impl the same way.
struct ConnCtx {
  std::shared_ptr<lt::session> session;
  lt::torrent_handle th;
  std::string file_path;
  std::uint64_t file_base = 0;
  std::uint64_t file_size = 0;
  int piece_len = 0;
  std::shared_ptr<std::atomic<bool>> stopped;
};

// Blocks until piece `p` is on disk. The demuxer is waiting on exactly this
// piece, so it gets a piece deadline (libtorrent streaming): the picker
// pulls it out of order ahead of the sequential front. Seek-backs inside
// already-downloaded pieces return immediately.
bool waitForPiece(const ConnCtx& ctx, int piece) {
  const lt::piece_index_t pi{piece};
  if (ctx.th.have_piece(pi)) return true;
  ctx.th.set_piece_deadline(pi, 0);
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(kPieceWaitMs);
  while (!ctx.stopped->load(std::memory_order_relaxed)) {
    if (ctx.th.have_piece(pi)) return true;
    if (std::chrono::steady_clock::now() >= deadline) return false;
    sleepMs(kPollMs);
  }
  return false;
}

// Serves [begin, end] (inclusive, clamped by the caller). Ranges ahead of
// the download front block on piece deadlines; everything else is served
// straight from disk without touching the network.
bool streamRange(const ConnCtx& ctx, int fd, std::uint64_t begin, std::uint64_t end) {
  std::uint8_t buf[kReadChunk];
  std::uint64_t pos = begin;
  while (pos <= end) {
    if (ctx.stopped->load(std::memory_order_relaxed)) return false;
    const size_t want =
        static_cast<size_t>(std::min<std::uint64_t>(sizeof(buf), end - pos + 1));
    const int piece0 = static_cast<int>((ctx.file_base + pos) / ctx.piece_len);
    const int piece1 =
        static_cast<int>((ctx.file_base + pos + want - 1) / ctx.piece_len);
    for (int p = piece0; p <= piece1; ++p) {
      if (!waitForPiece(ctx, p)) return false;
    }
    if (preadFile(ctx.file_path, ctx.file_base + pos, buf, want) != want) return false;
    if (!sendAll(fd, reinterpret_cast<const char*>(buf), want)) return false;
    pos += want;
  }
  return true;
}

// Case-insensitive header lookup in the raw request head (which still has
// its leading whitespace, hence the trim).
static std::string headerValue(const std::string& head, const char* name) {
  std::string lower = head;
  for (char& c : lower) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  const std::string key = std::string("\r\n") + name;
  const size_t at = lower.find(key);
  if (at == std::string::npos) return "";
  size_t v = at + key.size();
  while (v < head.size() && (head[v] == ' ' || head[v] == '\t')) ++v;
  size_t e = head.find("\r\n", v);
  if (e == std::string::npos) e = head.size();
  return head.substr(v, e - v);
}

// "bytes=A-B" | "bytes=A-" | "bytes=-N" → [first, last] within the file.
// All three forms matter: libavformat's http demuxer uses open-ended and
// suffix probes. Returns false when absent or malformed.
static bool parseRange(const std::string& value, std::uint64_t size,
                       std::uint64_t* first, std::uint64_t* last) {
  const std::string prefix = "bytes=";
  const size_t b = value.find_first_not_of(" \t");
  if (b == std::string::npos || value.compare(b, prefix.size(), prefix) != 0) return false;
  const std::string spec = value.substr(b + prefix.size());
  const size_t dash = spec.find('-');
  if (dash == std::string::npos) return false;
  const std::string a = spec.substr(0, dash);
  const std::string z = spec.substr(dash + 1);
  if (a.empty()) {
    if (z.empty() || z.find_first_not_of("0123456789") != std::string::npos) return false;
    const std::uint64_t n = std::strtoull(z.c_str(), nullptr, 10);
    if (n == 0 || n >= size) return false;
    *first = size - n;
    *last = size - 1;
  } else {
    if (a.find_first_not_of("0123456789") != std::string::npos) return false;
    *first = std::strtoull(a.c_str(), nullptr, 10);
    if (z.empty()) {
      *last = size - 1;
    } else {
      if (z.find_first_not_of("0123456789") != std::string::npos) return false;
      *last = std::strtoull(z.c_str(), nullptr, 10);
    }
  }
  if (size == 0 || *first >= size) return false;
  *last = std::min(*last, size - 1);
  return true;
}

// One HTTP connection: request/response loop (keep-alive until the client
// goes away or the stream stops). Bodies stream through streamRange(), so
// every read blocks exactly as long as its pieces need.
void handleConn(std::shared_ptr<ConnCtx> ctx, int fd) {
  std::string buf;  // leftover bytes between pipelined requests
  while (!ctx->stopped->load(std::memory_order_relaxed)) {
    // Read one request head.
    size_t head_end = buf.find("\r\n\r\n");
    while (head_end == std::string::npos) {
      if (buf.size() >= kMaxHeadBytes) {
        sockClose(fd);
        return;
      }
      char tmp[4096];
      const int r = sockRecv(fd, tmp, static_cast<int>(sizeof(tmp)));
      if (r <= 0) {
        sockClose(fd);
        return;
      }
      buf.append(tmp, static_cast<size_t>(r));
      head_end = buf.find("\r\n\r\n");
    }

    const std::string head = buf.substr(0, head_end);
    buf.erase(0, head_end + 4);

    // Request line: METHOD SP request-target SP HTTP/x.x
    const size_t line_end = head.find("\r\n");
    const std::string line =
        head.substr(0, line_end == std::string::npos ? head.size() : line_end);
    const size_t sp1 = line.find(' ');
    if (sp1 == std::string::npos) break;
    const std::string method = line.substr(0, sp1);
    if (method != "GET" && method != "HEAD") {
      static const char kResp[] =
          "HTTP/1.1 405 Method Not Allowed\r\nAllow: GET, HEAD\r\nConnection: close\r\n\r\n";
      sendAll(fd, kResp, sizeof(kResp) - 1);
      break;
    }

    std::uint64_t first = 0, last = 0;
    bool has_range = false;
    const std::string range = headerValue(head, "range:");
    if (!range.empty()) {
      if (!parseRange(range, ctx->file_size, &first, &last)) {
        const std::string resp = "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */" +
                                 std::to_string(ctx->file_size) + "\r\nConnection: close\r\n\r\n";
        sendAll(fd, resp.data(), resp.size());
        break;
      }
      has_range = true;
    } else {
      last = ctx->file_size - 1;
    }

    std::string resp = has_range ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n";
    resp += "Content-Type: application/octet-stream\r\nAccept-Ranges: bytes\r\n";
    resp += "Server: soar-p2p/1\r\nConnection: keep-alive\r\n";
    if (has_range) {
      resp += "Content-Range: bytes " + std::to_string(first) + "-" + std::to_string(last) +
              "/" + std::to_string(ctx->file_size) + "\r\n";
    }
    resp += "Content-Length: " + std::to_string(last - first + 1) + "\r\n\r\n";
    if (!sendAll(fd, resp.data(), resp.size())) break;
    if (method == "HEAD") continue;
    if (!streamRange(*ctx, fd, first, last)) break;
  }
  sockClose(fd);
}

}  // namespace

struct TorrentStream::Impl {
  Params params;
  std::shared_ptr<lt::session> session;
  lt::torrent_handle th;
  std::string file_path;  // absolute path of the served file inside store_dir
  std::uint64_t file_base = 0;
  std::uint64_t file_size = 0;
  int piece_len = 0;

  int listen_fd = -1;
  std::thread accept_thread;
  std::thread monitor_thread;
  // shared_ptr: connection threads outlive stop() and must not dereference
  // a deleted Impl to learn about shutdown.
  std::shared_ptr<std::atomic<bool>> stopped = std::make_shared<std::atomic<bool>>(false);

  mutable std::mutex status_mu;
  TorrentStatus status;

  std::shared_ptr<ConnCtx> makeCtx() const {
    auto ctx = std::make_shared<ConnCtx>();
    ctx->session = session;
    ctx->th = th;
    ctx->file_path = file_path;
    ctx->file_base = file_base;
    ctx->file_size = file_size;
    ctx->piece_len = piece_len;
    ctx->stopped = stopped;
    return ctx;
  }

  void monitorLoop() {
    const std::shared_ptr<lt::session> session = this->session;
    const lt::torrent_handle th = this->th;
    auto last_cb = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    while (!stopped->load(std::memory_order_relaxed)) {
      sleepMs(500);
      if (stopped->load(std::memory_order_relaxed)) break;
      const lt::torrent_status st = th.status();
      TorrentStatus s;
      s.downloaded = static_cast<std::uint64_t>(st.total_done);
      s.total = static_cast<std::uint64_t>(st.total_wanted);
      s.peers = st.num_peers;
      // A .torrent entry point carries its metadata locally (valid right
      // after add); a magnet only has it once the swarm delivered it.
      s.metadata = th.torrent_file() != nullptr;
      {
        std::lock_guard<std::mutex> lock(status_mu);
        status = s;
      }
      const auto now = std::chrono::steady_clock::now();
      if (params.on_progress && now - last_cb >= std::chrono::milliseconds(900)) {
        last_cb = now;
        params.on_progress(s);
      }
    }
  }

  void acceptLoop() {
    while (!stopped->load(std::memory_order_relaxed)) {
      const int cfd = sockAccept(listen_fd);
      if (cfd < 0) {
        if (stopped->load(std::memory_order_relaxed)) break;
        continue;
      }
      std::thread(handleConn, makeCtx(), cfd).detach();
    }
  }
};

TorrentStream::~TorrentStream() { stop(); }

bool TorrentStream::start(const Params& params) {
  if (impl_ != nullptr) {
    last_error_ = "torrent: already started";
    return false;
  }
  const bool is_magnet = !params.magnet_uri.empty();
  if (params.torrent_path.empty() && !is_magnet) {
    last_error_ = "torrent: no .torrent path or magnet URI given";
    return false;
  }
  if (params.store_dir.empty()) {
    last_error_ = "torrent: no store dir given";
    return false;
  }

  auto impl = std::make_unique<Impl>();
  impl->params = params;

  lt::add_torrent_params atp;
  if (is_magnet) {
    // libtorrent accepts btih as 40-hex or 32-base32 and fills trackers
    // (tr=), the display name (dn=), in-magnet peers (x.pe=) and DHT nodes
    // (dht=). The session's default bootstrap nodes make the hash alone
    // enough to find peers on the public DHT.
    lt::error_code ec;
    lt::parse_magnet_uri(params.magnet_uri, atp, ec);
    if (ec) {
      last_error_ = "torrent: cannot parse magnet URI: " + ec.message();
      return false;
    }
  } else {
    try {
      atp = lt::load_torrent_file(params.torrent_path);
    } catch (const std::exception& e) {
      last_error_ = std::string("torrent: cannot parse ") + params.torrent_path + ": " + e.what();
      return false;
    }
  }

  lt::settings_pack sp;
  sp.set_str(lt::settings_pack::listen_interfaces, "0.0.0.0:6881,[::]:6881");
  lt::session_params sparams;
  sparams.settings = std::move(sp);
  impl->session = std::make_shared<lt::session>(sparams);

  atp.save_path = params.store_dir;
  // Sequential download feeds linear playback from piece 0 on; reads ahead
  // of the front (an mp4 moov near the tail, seeks) turn into per-piece
  // deadlines inside waitForPiece().
  atp.flags |= lt::torrent_flags::sequential_download;
  try {
    impl->th = impl->session->add_torrent(atp);
  } catch (const std::exception& e) {
    last_error_ = std::string("torrent: cannot start download: ") + e.what();
    return false;
  }

  for (const std::string& peer : params.peers) {
    const size_t colon = peer.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= peer.size()) {
      last_error_ = "torrent: bad peer (want host:port): " + peer;
      return false;
    }
    const std::string host = peer.substr(0, colon);
    const int port = std::atoi(peer.c_str() + colon + 1);
    if (port <= 0 || port > 65535) {
      last_error_ = "torrent: bad peer port in " + peer;
      return false;
    }
    try {
      const lt::address addr = lt::make_address(host);
      impl->th.connect_peer(lt::tcp::endpoint(addr, static_cast<unsigned short>(port)));
    } catch (const std::exception& e) {
      last_error_ = "torrent: bad peer " + host + ": " + e.what();
      return false;
    }
  }

  // A magnet starts without the metadata (info dictionary); the swarm has
  // to deliver it before any file geometry exists. A .torrent carries it
  // locally, so torrent_file() is valid right after add and this wait is a
  // no-op there.
  const auto meta_deadline = std::chrono::steady_clock::now() +
                             std::chrono::milliseconds(kMetadataWaitMs);
  while (impl->th.torrent_file() == nullptr) {
    if (std::chrono::steady_clock::now() >= meta_deadline) {
      last_error_ = "torrent: timed out waiting for metadata (peers seen: " +
                    std::to_string(impl->th.status().num_peers) + ")";
      return false;
    }
    sleepMs(kPollMs);
  }

  const std::shared_ptr<const lt::torrent_info> ti = impl->th.torrent_file();
  const lt::file_storage& fs = ti->files();
  const int num_files = fs.num_files();
  int index = params.file_index;
  if (num_files == 1) index = 0;
  if (index < 0 || index >= num_files) {
    last_error_ = "torrent: file index " + std::to_string(index) + " out of range (torrent has " +
                  std::to_string(num_files) + " files)";
    return false;
  }
  const lt::file_index_t fi{index};
  impl->file_base = static_cast<std::uint64_t>(fs.file_offset(fi));
  impl->file_size = static_cast<std::uint64_t>(fs.file_size(fi));
  impl->piece_len = ti->piece_length();
  if (impl->file_size == 0 || impl->piece_len == 0) {
    last_error_ = "torrent: file is empty";
    return false;
  }
  impl->file_path = params.store_dir + "/" + fs.file_path(fi, "");
  file_name_ = std::string(fs.file_name(fi));
  file_size_ = impl->file_size;

  sockInit();

  const int listen_fd = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
  if (listen_fd < 0) {
    last_error_ = "torrent: cannot create listen socket (errno " +
                  std::to_string(lastNetError()) + ")";
    return false;
  }
  int one = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
             reinterpret_cast<const char*>(&one), sizeof(one));
  sockaddr_in bind_addr{};
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // never exposed off-host
  bind_addr.sin_port = 0;  // any free port; the URL carries the chosen one
  if (bind(listen_fd, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0 ||
      listen(listen_fd, kListenBacklog) != 0) {
    last_error_ = "torrent: cannot listen on 127.0.0.1 (errno " +
                  std::to_string(lastNetError()) + ")";
    sockClose(listen_fd);
    return false;
  }
  sockaddr_in bound{};
  socklen_t blen = sizeof(bound);
  getsockname(listen_fd, reinterpret_cast<sockaddr*>(&bound), &blen);
  playback_url_ = "http://127.0.0.1:" + std::to_string(ntohs(bound.sin_port)) + "/";

  impl->listen_fd = listen_fd;
  impl->accept_thread = std::thread([impl = impl.get()] { impl->acceptLoop(); });
  impl->monitor_thread = std::thread([impl = impl.get()] { impl->monitorLoop(); });
  impl_ = impl.release();
  return true;
}

void TorrentStream::stop() {
  if (impl_ == nullptr) return;
  std::unique_ptr<Impl> impl(impl_);
  impl_ = nullptr;
  impl->stopped->store(true, std::memory_order_relaxed);
  if (impl->listen_fd >= 0) {
    sockShutdown(impl->listen_fd);
    sockClose(impl->listen_fd);
    impl->listen_fd = -1;
  }
  if (impl->accept_thread.joinable()) impl->accept_thread.join();
  if (impl->monitor_thread.joinable()) impl->monitor_thread.join();
  // Connection threads hold their own copies (ConnCtx::session); libtorrent
  // dies when the last of them is done with it.
  impl->session.reset();
}

TorrentStatus TorrentStream::status() const {
  TorrentStatus s;
  if (impl_ == nullptr) return s;
  std::lock_guard<std::mutex> lock(impl_->status_mu);
  return impl_->status;
}

}  // namespace soar::p2p
