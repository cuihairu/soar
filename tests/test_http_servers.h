// Shared HTTP test-server fixtures: server scripts plus a pipe-based
// readiness handshake. Used by the media backend tests (HLS over http,
// network stall) and the disk-cache tests. POSIX-only: every user gates
// on _WIN32 and a python3 availability check before forking.

#ifndef SOAR_TEST_HTTP_SERVERS_H_
#define SOAR_TEST_HTTP_SERVERS_H_

#include <string>

#ifndef _WIN32
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace test_servers {

// Minimal python http.server that answers Range requests with proper
// 206 / Content-Range responses, serving files from one root directory.
// Announces readiness by printing "ready" once the socket is bound — the
// harness reads it over a pipe (see startPipedServer), so a stale server
// from an earlier run can never fake readiness.
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

srv = ThreadingHTTPServer(("127.0.0.1", int(sys.argv[2])), RangeHandler)
print("ready", flush=True)
srv.serve_forever()
)PY";

// Plain 200-only python http.server (no Range support): the negative
// fixture for "cache rejects range-less servers". Same pipe handshake.
constexpr const char* kPlainServerScript = R"PY(
import os, sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.abspath(sys.argv[1])

class PlainHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *args):
        pass
    def do_GET(self):
        path = self.path.split("?", 1)[0].split("#", 1)[0]
        target = os.path.normpath(os.path.join(ROOT, path.lstrip("/")))
        if not target.startswith(ROOT + os.sep) or not os.path.isfile(target):
            self.send_error(404)
            return
        with open(target, "rb") as f:
            data = f.read()
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

srv = ThreadingHTTPServer(("127.0.0.1", int(sys.argv[2])), PlainHandler)
print("ready", flush=True)
srv.serve_forever()
)PY";

#ifndef _WIN32
// Runs `argv` via fork/execvp with the child's stdout plumbed into a pipe
// and waits for its "ready" line (EOF = the server never came up, e.g. the
// port was taken). Returns the child pid, or -1 on any failure; the caller
// owns reaping the pid. Never probe a forked server's TCP port for
// readiness: a stale server from an earlier run on the same fixed port
// answers the probe and the test then fetches from the wrong process (404s
// or a completely different root) — the same failure class as the RTSP
// single-accept lesson, in multi-connection disguise.
inline pid_t startPipedServer(char* const argv[]) {
  int fds[2];
  if (::pipe(fds) != 0) return -1;
  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(fds[0]);
    ::close(fds[1]);
    return -1;
  }
  if (pid == 0) {
    ::dup2(fds[1], 1);
    ::close(fds[0]);
    ::close(fds[1]);
    ::execvp(argv[0], argv);
    _exit(127);
  }
  ::close(fds[1]);
  std::string line;
  char c = 0;
  while (::read(fds[0], &c, 1) == 1 && c != '\n') line += c;
  ::close(fds[0]);
  if (line != "ready") {
    ::kill(pid, SIGTERM);
    ::waitpid(pid, nullptr, 0);
    return -1;
  }
  return pid;
}

// A forked Range-server instance. The destructor always reaps the child:
// a REQUIRE failure jumps straight out of the case, and an orphaned
// server inherits the harness's stdout pipe — ctest then waits for EOF
// until the timeout even though the test binary already exited.
struct RangeServer {
  pid_t pid = -1;
  int port = 0;
  std::string base_url;
  ~RangeServer() { stop(); }
  void stop() {
    if (pid >= 0) {
      ::kill(pid, SIGTERM);
      ::waitpid(pid, nullptr, 0);
      pid = -1;
    }
  }
};

// Forks the shared Range-server script rooted at `root_dir`, listening on
// `port_base + getpid() % 200`; readiness comes from the pipe handshake.
inline RangeServer startRangeServer(const std::string& root_dir,
                                    int port_base) {
  RangeServer srv;
  srv.port = port_base + (::getpid() % 200);
  std::string port_str = std::to_string(srv.port);
  char* const argv[] = {
    const_cast<char*>("python3"),
    const_cast<char*>("-c"),
    const_cast<char*>(kRangeServerScript),
    const_cast<char*>(root_dir.c_str()),
    const_cast<char*>(port_str.c_str()),
    nullptr,
  };
  srv.pid = startPipedServer(argv);
  srv.base_url = "http://127.0.0.1:" + std::to_string(srv.port);
  return srv;
}
#endif  // !_WIN32

}  // namespace test_servers

#endif  // SOAR_TEST_HTTP_SERVERS_H_
