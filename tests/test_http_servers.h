// Shared HTTP test-server fixtures: a Range-capable server script plus a
// readiness probe. Used by the media backend tests (HLS over http, network
// stall) and the disk-cache tests. POSIX-only: every user gates on _WIN32
// and a python3 availability check before forking.

#ifndef SOAR_TEST_HTTP_SERVERS_H_
#define SOAR_TEST_HTTP_SERVERS_H_

// Minimal python http.server that answers Range requests with proper
// 206 / Content-Range responses, serving files from one root directory.
namespace test_servers {

constexpr const char* kRangeServerScript = R"PY(
import os, re, sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.abspath(sys.argv[1])

class RangeHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *args):
        pass
    def do_GET(self):
        path = self.path.split("?", 1)[0].split("#", 1)[0]
        target = os.path.normpath(os.path.join(ROOT, path.lstrip("/")))
        if not target.startswith(ROOT + os.sep) or not os.path.isfile(target):
            self.send_error(404)
            return
        size = os.path.getsize(target)
        start, end = 0, size - 1
        partial = False
        rng = self.headers.get("Range")
        if rng:
            m = re.match(r"bytes=(\d*)-(\d*)$", rng.strip())
            if m and (m.group(1) or m.group(2)):
                if m.group(1):
                    start = int(m.group(1))
                    if m.group(2):
                        end = min(int(m.group(2)), size - 1)
                else:
                    start = max(0, size - int(m.group(2)))
                partial = start <= end < size
        if start > end or start >= size:
            self.send_error(416)
            return
        length = end - start + 1
        self.send_response(206 if partial else 200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(length))
        self.send_header("Accept-Ranges", "bytes")
        if partial:
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
        self.end_headers()
        with open(target, "rb") as f:
            f.seek(start)
            remaining = length
            while remaining > 0:
                chunk = f.read(min(65536, remaining))
                if not chunk:
                    break
                self.wfile.write(chunk)
                remaining -= len(chunk)

ThreadingHTTPServer(("127.0.0.1", int(sys.argv[2])), RangeHandler).serve_forever()
)PY";

// TCP probe against 127.0.0.1:port. Safe with ThreadingHTTPServer (it
// accepts many connections); never use this with a single-accept server.
inline bool rangeServerReady(int port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return false;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  const bool ready = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
  ::close(fd);
  return ready;
}

}  // namespace test_servers

#endif  // SOAR_TEST_HTTP_SERVERS_H_
