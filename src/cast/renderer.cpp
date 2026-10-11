#include "renderer.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

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

#include "ssdp.h"
#include "text_util.h"

namespace soar::cast {

namespace {

// The wire budget of the control point: a device description or a SOAP
// reply is at most a few KiB; anything beyond this is treated as garbage
// rather than buffered.
constexpr size_t kMaxReply = 4 * 1024 * 1024;
constexpr int kConnectTimeoutMs = 4000;
constexpr int kIoTimeoutSec = 8;
constexpr int kSsdpGroupPort = 1900;
constexpr const char kSsdpGroup[] = "239.255.255.250";

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

// Bounded waits over select() — poll() flags do not exist there.
static bool waitReadable(int s, int ms) {
  fd_set set;
  FD_ZERO(&set);
  FD_SET(static_cast<SOCKET>(s), &set);
  timeval tv{};
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  return ::select(s + 1, &set, nullptr, nullptr, &tv) > 0;
}
static bool waitWritable(int s, int ms) {
  fd_set set;
  FD_ZERO(&set);
  FD_SET(static_cast<SOCKET>(s), &set);
  timeval tv{};
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  return ::select(s + 1, nullptr, &set, nullptr, &tv) > 0;
}
#else
static void sockInit() {}
static void sockClose(int s) { ::close(s); }
static int lastNetError() { return errno; }
static int sockSend(int s, const char* p, int n) {
  return static_cast<int>(::send(s, p, static_cast<size_t>(n), 0));
}
static int sockRecv(int s, char* p, int n) {
  return static_cast<int>(::recv(s, p, static_cast<size_t>(n), 0));
}
static bool waitReadable(int s, int ms) {
  pollfd p{};
  p.fd = s;
  p.events = POLLIN;
  return ::poll(&p, 1, ms) > 0 && (p.revents & POLLIN) != 0;
}
static bool waitWritable(int s, int ms) {
  pollfd p{};
  p.fd = s;
  p.events = POLLOUT;
  return ::poll(&p, 1, ms) > 0 && (p.revents & POLLOUT) != 0;
}
#endif

bool sendAll(int fd, const char* p, size_t n) {
  while (n > 0) {
    const int w = sockSend(fd, p, static_cast<int>(std::min<size_t>(n, 1 << 20)));
    if (w <= 0) return false;
    p += w;
    n -= static_cast<size_t>(w);
  }
  return true;
}

// Connects (host may be a dotted quad or a name) with a bounded connect
// wait — a dead renderer must cost seconds, not a full TCP SYN timeout.
// Applies the IO timeouts on success. Returns -1 on any failure.
int connectTo(const std::string& host, int port, std::string* err) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* list = nullptr;
  if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &list) != 0 ||
      list == nullptr) {
    *err = "cast: cannot resolve " + host;
    return -1;
  }
  std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> guard(list, &::freeaddrinfo);

  const int fd = static_cast<int>(
      ::socket(list->ai_family, list->ai_socktype, list->ai_protocol));
  if (fd < 0) {
    *err = "cast: cannot create socket (errno " + std::to_string(lastNetError()) + ")";
    return -1;
  }
#ifdef _WIN32
  u_long nonblocking = 1;
  ::ioctlsocket(fd, FIONBIO, &nonblocking);
#else
  const int flags = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
  if (::connect(fd, list->ai_addr, static_cast<socklen_t>(list->ai_addrlen)) != 0 &&
      !waitWritable(fd, kConnectTimeoutMs)) {
    *err = "cast: cannot reach " + host + ":" + std::to_string(port) +
           " (connect timeout)";
    sockClose(fd);
    return -1;
  }
  int so_err = 0;
  socklen_t len = sizeof(so_err);
  ::getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_err), &len);
  if (so_err != 0) {
    *err = "cast: cannot reach " + host + ":" + std::to_string(port) + " (errno " +
           std::to_string(so_err) + ")";
    sockClose(fd);
    return -1;
  }
  // Back to blocking: the IO timeouts below (SO_RCVTIMEO/SNDTIMEO) only
  // apply to a blocking socket, and every later recv/send assumes them.
#ifdef _WIN32
  u_long blocking = 0;
  ::ioctlsocket(fd, FIONBIO, &blocking);
#else
  ::fcntl(fd, F_SETFL, flags);
#endif
  timeval tv{};
  tv.tv_sec = kIoTimeoutSec;
  tv.tv_usec = 0;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv),
               sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv),
               sizeof(tv));
  return fd;
}

// One HTTP exchange on a fresh connection (every request is
// "Connection: close"): send the request text, read the reply. A reply
// with Content-Length is read exactly; one without is read to EOF (the
// per-recv timeout bounds a server that never closes). Returns the reply
// status and body; `err` explains any failure.
bool httpExchange(const UrlParts& parts, const std::string& request, int* status,
                  std::string* body, std::string* err) {
  *status = 0;
  body->clear();
  const int fd = connectTo(parts.host, parts.port, err);
  if (fd < 0) return false;

  bool ok = sendAll(fd, request.data(), request.size());
  std::string head, reply;
  char buf[8192];
  while (ok && head.find("\r\n\r\n") == std::string::npos) {
    const int r = sockRecv(fd, buf, static_cast<int>(sizeof(buf)));
    if (r <= 0) {
      ok = false;
      break;
    }
    head.append(buf, static_cast<size_t>(r));
    if (head.size() > kMaxReply) ok = false;
  }
  size_t content_length = 0;
  if (ok) {
    const size_t head_end = head.find("\r\n\r\n");
    reply = head.substr(head_end + 4);
    head.resize(head_end);
    const std::string length_field = fieldValue(head, "CONTENT-LENGTH");
    if (!length_field.empty()) {
      content_length = static_cast<size_t>(
          std::strtoull(length_field.c_str(), nullptr, 10));
      if (content_length > kMaxReply) {
        *err = "cast: reply too large from " + parts.host;
        sockClose(fd);
        return false;
      }
    }
    while (reply.size() < content_length ||
           (content_length == 0 && ok)) {
      const int r = sockRecv(fd, buf, static_cast<int>(sizeof(buf)));
      if (r <= 0) break;  // clean close or timeout ends a no-length body
      reply.append(buf, static_cast<size_t>(r));
      if (reply.size() > kMaxReply) {
        ok = false;
        break;
      }
    }
  }
  sockClose(fd);
  if (!ok) {
    *err = "cast: reply read failed from " + parts.host + ":" +
           std::to_string(parts.port);
    return false;
  }
  if (content_length > 0 && reply.size() > content_length) {
    reply.resize(content_length);  // spill of a pipelined extra reply
  }
  *status = httpStatus(head);
  if (*status == 0) {
    *err = "cast: not an HTTP reply from " + parts.host + ":" +
           std::to_string(parts.port);
    return false;
  }
  *body = std::move(reply);
  return true;
}

std::string hostPort(const UrlParts& parts) {
  return parts.host + ":" + std::to_string(parts.port);
}

// Shared body of the four AVTransport actions: 200 is success, anything
// else becomes a SOAP-fault or HTTP-status error message.
bool actionReply(const std::string& control_url, const std::string& request,
                 std::string* err) {
  int status = 0;
  std::string body;
  if (control_url.empty()) {
    *err = "cast: no control URL";
    return false;
  }
  UrlParts parts;
  if (!splitUrl(control_url, &parts)) {
    *err = "cast: bad control URL " + control_url;
    return false;
  }
  if (!httpExchange(parts, request, &status, &body, err)) return false;
  if (status != 200) {
    SoapFault fault;
    if (parseFault(body, &fault)) {
      *err = "SOAP fault " + std::to_string(fault.code) +
             (fault.description.empty() ? "" : ": " + fault.description);
    } else {
      *err = "renderer answered HTTP " + std::to_string(status);
    }
    return false;
  }
  return true;
}

}  // namespace

int discoverRenderers(int timeout_ms, std::vector<DiscoveredRenderer>* out,
                      std::string* err, const char* multicast_if) {
  if (out == nullptr || err == nullptr) return -1;
  out->clear();
  *err = "";
  sockInit();
  const int fd = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, 0));
  if (fd < 0) {
    *err = "ssdp: cannot create socket (errno " + std::to_string(lastNetError()) + ")";
    return -1;
  }
  // Fully non-blocking: every wait below is poll-driven, and a blocking
  // sendto on a wedged route/interface would hang the discovery window
  // past its own deadline.
#ifdef _WIN32
  u_long nonblocking = 1;
  ::ioctlsocket(fd, FIONBIO, &nonblocking);
#else
  {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
#endif
  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  local.sin_port = 0;  // replies arrive at the ephemeral source port
  if (::bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
    *err = "ssdp: cannot bind (errno " + std::to_string(lastNetError()) + ")";
    sockClose(fd);
    return -1;
  }
  if (multicast_if != nullptr) {
    in_addr if_addr{};
    if (::inet_pton(AF_INET, multicast_if, &if_addr) != 1) {
      *err = std::string("ssdp: bad multicast interface ") + multicast_if;
      sockClose(fd);
      return -1;
    }
    ::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF,
                 reinterpret_cast<const char*>(&if_addr), sizeof(if_addr));
    // The in-process tests rely on hearing this host's own burst back.
    const unsigned char loop = 1;
    ::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP,
                 reinterpret_cast<const char*>(&loop), sizeof(loop));
  }

  sockaddr_in group{};
  group.sin_family = AF_INET;
  group.sin_port = htons(static_cast<unsigned short>(kSsdpGroupPort));
  ::inet_pton(AF_INET, kSsdpGroup, &group.sin_addr);

  const std::string search = makeSearchRequest(kAvTransportType, 2);
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  auto last_send = std::chrono::steady_clock::now() - std::chrono::seconds(1);
  bool resent = false;
  std::vector<DiscoveredRenderer> found;
  // Two bursts 300ms apart: some stacks answer unreliably to one packet.
  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) break;
    if (!resent && now - last_send >= std::chrono::milliseconds(300)) {
      ::sendto(fd, search.data(), static_cast<int>(search.size()), 0,
               reinterpret_cast<const sockaddr*>(&group), sizeof(group));
      resent = true;
    }
    const int remaining = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
    if (!waitReadable(fd, (std::min)(remaining, 250))) continue;
    char buf[4096];
    const int r = static_cast<int>(::recvfrom(fd, buf, sizeof(buf), 0, nullptr, nullptr));
    if (r <= 0) continue;
    SearchResponse resp;
    if (!parseSearchResponse(std::string_view(buf, static_cast<size_t>(r)), &resp))
      continue;
    const bool seen = std::any_of(found.begin(), found.end(), [&](const auto& d) {
      return d.usn == resp.usn || d.location == resp.location;
    });
    if (seen) continue;
    DiscoveredRenderer d;
    d.location = resp.location;
    d.usn = resp.usn;
    found.push_back(std::move(d));
  }
  sockClose(fd);

  // Friendly names are a courtesy: a description that fails to load leaves
  // an empty name but a usable location.
  std::string scratch;
  for (DiscoveredRenderer& d : found) {
    DeviceDesc desc;
    if (fetchDescription(d.location, &desc, &scratch)) {
      d.friendly_name = desc.friendly_name;
    }
  }
  *out = std::move(found);
  return static_cast<int>(out->size());
}

bool fetchDescription(const std::string& location, DeviceDesc* out, std::string* err) {
  UrlParts parts;
  if (!splitUrl(location, &parts)) {
    if (err != nullptr) *err = "cast: bad description URL " + location;
    return false;
  }
  std::string request;
  request += "GET " + (parts.path.empty() ? std::string("/") : parts.path) + " HTTP/1.1\r\n";
  request += "HOST: " + hostPort(parts) + "\r\n";
  request += "USER-AGENT: soar-cast/1\r\n";
  request += "Connection: close\r\n\r\n";
  int status = 0;
  std::string body;
  std::string local_err;
  if (!httpExchange(parts, request, &status, &body, err != nullptr ? err : &local_err)) {
    return false;
  }
  if (status != 200) {
    if (err != nullptr)
      *err = "cast: description HTTP " + std::to_string(status) + " from " + hostPort(parts);
    return false;
  }
  if (!parseDeviceDescription(body, out)) {
    if (err != nullptr) *err = "cast: not a device description from " + hostPort(parts);
    return false;
  }
  return true;
}

bool RendererClient::setUri(const std::string& uri, const std::string& title,
                            std::string* err) {
  return actionReply(control_url_,
                     cast::setAvTransportUri(control_url_, uri, title), err);
}

bool RendererClient::play(std::string* err) {
  // Qualified: unqualified `play` inside this member names the member.
  return actionReply(control_url_, cast::play(control_url_), err);
}

bool RendererClient::stop(std::string* err) {
  return actionReply(control_url_, cast::stop(control_url_), err);
}

bool RendererClient::seekRel(std::int64_t ms, std::string* err) {
  return actionReply(control_url_, cast::seekRelTime(control_url_, ms), err);
}

bool RendererClient::pollPosition(PositionInfo* out, std::string* err) {
  int status = 0;
  std::string body;
  if (control_url_.empty()) {
    *err = "cast: no control URL";
    return false;
  }
  UrlParts parts;
  if (!splitUrl(control_url_, &parts)) {
    *err = "cast: bad control URL " + control_url_;
    return false;
  }
  if (!httpExchange(parts, getPositionInfo(control_url_), &status, &body, err)) {
    return false;
  }
  if (status != 200) {
    *err = "renderer answered HTTP " + std::to_string(status);
    return false;
  }
  if (!parsePositionInfo(body, out)) {
    *err = "renderer sent no position report";
    return false;
  }
  return true;
}

}  // namespace soar::cast
