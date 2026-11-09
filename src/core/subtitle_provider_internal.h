// subtitle_provider_internal.h: the socket/HTTP primitives
// shared by the subtitle download client and the translator
// client - URL parsing, the TCP connect/read/write helpers
// and the winsock shims. Internal to the subtitle_provider
// translation units; not installed. Split out of
// subtitle_provider.cpp so the two clients stay separate
// files without sharing a header they do not both need.
//
// Note: these helpers have internal linkage per translation
// unit (anonymous namespace in a header). winsockInit's
// once-guard is therefore per TU: on Windows each client TU
// calls WSAStartup once. WSAStartup is refcounted and the
// process never calls WSACleanup, so behaviour is unchanged.

#ifndef SOAR_CORE_SUBTITLE_PROVIDER_INTERNAL_H_
#define SOAR_CORE_SUBTITLE_PROVIDER_INTERNAL_H_

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


std::string toLower(const std::string& s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

constexpr size_t kMaxDownloadBytes = 8 * 1024 * 1024;  // search body or subtitle


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

} // namespace

} // namespace soar

#endif  // SOAR_CORE_SUBTITLE_PROVIDER_INTERNAL_H_
