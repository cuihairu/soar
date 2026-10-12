#include "event_server.h"

#include <cstdlib>
#include <cstring>
#include <memory>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

#include "text_util.h"

namespace soar::cast {

namespace {

#ifdef _WIN32
using socklen_t = int;

void sockInit() {
  static const bool ready = [] {
    WSADATA d;
    return WSAStartup(MAKEWORD(2, 2), &d) == 0;
  }();
  (void)ready;
}
void sockClose(int s) { closesocket(s); }
int lastNetError() { return WSAGetLastError(); }
int sockRecv(int s, char* p, int n) { return recv(s, p, n, 0); }
int sockSend(int s, const char* p, int n) { return send(s, p, n, 0); }
#else
void sockInit() {}
void sockClose(int s) { ::close(s); }
int lastNetError() { return errno; }
int sockRecv(int s, char* p, int n) {
  return static_cast<int>(::recv(s, p, static_cast<size_t>(n), 0));
}
int sockSend(int s, const char* p, int n) {
  return static_cast<int>(::send(s, p, static_cast<size_t>(n), 0));
}
#endif

// Readiness wait for both the accept loop and the per-connection reads —
// select() on Windows (no poll flags there), poll() elsewhere.
bool waitReadable(int s, int ms) {
#ifdef _WIN32
  fd_set set;
  FD_ZERO(&set);
  FD_SET(static_cast<SOCKET>(s), &set);
  timeval tv{};
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  return ::select(s + 1, &set, nullptr, nullptr, &tv) > 0;
#else
  pollfd p{};
  p.fd = s;
  p.events = POLLIN;
  return ::poll(&p, 1, ms) > 0 && (p.revents & POLLIN) != 0;
#endif
}

bool sendAll(int fd, const char* p, size_t n) {
  while (n > 0) {
    const int w = sockSend(fd, p, static_cast<int>(n));
    if (w <= 0) return false;
    p += w;
    n -= static_cast<size_t>(w);
  }
  return true;
}

constexpr size_t kMaxNotify = 64 * 1024;  // event bodies are small; cap anyway
constexpr int kAcceptPollMs = 250;        // how often accept checks `running_`
constexpr int kReadTimeoutMs = 5000;      // one NOTIFY must arrive in this

// The local dotted-quad address whose interface routes toward `host` (the
// UDP-connect trick — connect a datagram socket, nothing is ever sent,
// read the local end). A copy of the p2p bridge's probe: the cast socket
// layer stays self-contained.
std::string localIpForPeer(const std::string& host) {
  sockaddr_in dst{};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(9);  // discard port makes source selection concrete
  if (::inet_pton(AF_INET, host.c_str(), &dst.sin_addr) != 1) {
    if (host.empty()) return "";
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_NUMERICSERV;
    addrinfo* list = nullptr;
    if (getaddrinfo(host.c_str(), "9", &hints, &list) != 0 || list == nullptr)
      return "";
    std::memcpy(&dst, list->ai_addr, sizeof(dst));
    freeaddrinfo(list);
  }
  const int fd = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, 0));
  if (fd < 0) return "";
  std::string out;
  if (::connect(fd, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) == 0) {
    sockaddr_in local{};
    socklen_t len = sizeof(local);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
      char buf[INET_ADDRSTRLEN] = {0};
      if (::inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf)) != nullptr)
        out = buf;
    }
  }
  sockClose(fd);
  return out;
}

// Reads one NOTIFY off an accepted connection: the head to its blank
// line, then Content-Length bytes of body. The callback fires with the
// body only when a well-formed request arrived; either way the reply is
// a bare 200 (a NOTIFY is fire-and-forget for the renderer — a retry
// would only duplicate an event the poll path covers).
void serveConnection(int fd, const std::function<void(std::string_view)>& cb) {
  std::string head, body;
  char buf[4096];
  bool ok = true;
  while (ok && head.find("\r\n\r\n") == std::string::npos &&
         head.size() <= kMaxNotify) {
    if (!waitReadable(fd, kReadTimeoutMs)) {
      ok = false;
      break;
    }
    const int r = sockRecv(fd, buf, static_cast<int>(sizeof(buf)));
    if (r <= 0) {
      ok = false;
      break;
    }
    head.append(buf, static_cast<size_t>(r));
  }
  if (ok) {
    const size_t head_end = head.find("\r\n\r\n");
    body = head.substr(head_end + 4);
    head.resize(head_end);
    const std::string length_field = fieldValue(head, "CONTENT-LENGTH");
    size_t content_length =
        length_field.empty()
            ? 0
            : static_cast<size_t>(std::strtoull(length_field.c_str(), nullptr, 10));
    if (content_length > kMaxNotify) content_length = kMaxNotify;
    while (body.size() < content_length) {
      if (!waitReadable(fd, kReadTimeoutMs)) break;
      const int r = sockRecv(fd, buf, static_cast<int>(sizeof(buf)));
      if (r <= 0) break;
      body.append(buf, static_cast<size_t>(r));
    }
    if (!body.empty() && cb) cb(body);
  }
  const char reply[] = "HTTP/1.1 200 OK\r\nCONTENT-LENGTH: 0\r\nCONNECTION: close\r\n\r\n";
  sendAll(fd, reply, sizeof(reply) - 1);
  sockClose(fd);
}

}  // namespace

EventServer::~EventServer() { stop(); }

bool EventServer::start(std::function<void(std::string_view)> on_notify,
                        std::string* err) {
  if (running_.load(std::memory_order_acquire)) return true;
  sockInit();
  const int fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
  if (fd < 0) {
    *err = "cast: event server cannot create socket (errno " +
           std::to_string(lastNetError()) + ")";
    return false;
  }
  const int reuse = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
               sizeof(reuse));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = 0;  // ephemeral: the callback URL is built after bind
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
      ::listen(fd, 4) != 0) {
    *err = "cast: event server cannot bind (errno " +
           std::to_string(lastNetError()) + ")";
    sockClose(fd);
    return false;
  }
  sockaddr_in bound{};
  socklen_t len = sizeof(bound);
  ::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len);
  listen_fd_ = fd;
  port_ = ntohs(bound.sin_port);
  on_notify_ = std::move(on_notify);
  running_.store(true, std::memory_order_release);
  serve_thread_ = std::thread([this] { serveLoop(); });
  return true;
}

void EventServer::stop() {
  if (!running_.exchange(false, std::memory_order_acq_rel)) return;
  // shutdown wakes a blocked accept on POSIX; Darwin ignores shutdown on
  // listeners, so the close itself is the wake there.
  if (listen_fd_ >= 0) {
#ifdef _WIN32
    ::shutdown(listen_fd_, SD_BOTH);
#else
    ::shutdown(listen_fd_, SHUT_RDWR);
#endif
    sockClose(listen_fd_);
    listen_fd_ = -1;
  }
  if (serve_thread_.joinable()) serve_thread_.join();
  port_ = 0;
  on_notify_ = nullptr;
}

std::string EventServer::callbackUrlFor(const std::string& peer_host) const {
  if (port_ == 0) return "";
  const std::string local = localIpForPeer(peer_host);
  if (local.empty()) return "";
  return "http://" + local + ":" + std::to_string(port_) + "/cast/events";
}

void EventServer::serveLoop() {
  for (;;) {
    if (!running_.load(std::memory_order_acquire)) break;
    if (!waitReadable(listen_fd_, kAcceptPollMs)) continue;
    const int fd = static_cast<int>(::accept(listen_fd_, nullptr, nullptr));
    if (fd < 0) {
      if (!running_.load(std::memory_order_acquire)) break;
      continue;
    }
    auto cb = on_notify_;
    serveConnection(fd, cb);
  }
}

}  // namespace soar::cast
