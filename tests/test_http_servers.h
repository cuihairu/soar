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
import os, re, socket, sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.abspath(sys.argv[1])
# Optional 3rd arg "v6": bind ::1 instead (the IPv6-literal url test).
V6 = len(sys.argv) > 3 and sys.argv[3] == "v6"

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

class V6RangeServer(ThreadingHTTPServer):
    address_family = socket.AF_INET6

srv = (V6RangeServer if V6 else ThreadingHTTPServer)(
    ("::1" if V6 else "127.0.0.1", int(sys.argv[2])), RangeHandler)
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
// Serves one file but stalls mid-body for 40s: the client's playback
// consumes the first burst in a couple of wall-clock seconds, then its
// reads go quiet, which is what the backend's network stall watchdog
// (the AVIO interrupt callback) reports as BufferingStarted/Ended. The
// pause must comfortably exceed the 10s report threshold under any
// instrumentation slowdown, so it is 40s. Used by the media suite (the
// event contract) and the CLI suite (the window's buffering indicator).
// Announces "ready" on stdout once bound, like the range server.
constexpr const char* kThrottledServerScript = R"PY(
import sys, time
from http.server import BaseHTTPRequestHandler, HTTPServer

with open(sys.argv[1], "rb") as f:
    data = f.read()
split = len(data) * 2 // 5

class ThrottledHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data[:split])
        self.wfile.flush()
        time.sleep(40)
        self.wfile.write(data[split:])
        self.wfile.flush()
    def log_message(self, *args):
        pass

srv = HTTPServer(("127.0.0.1", int(sys.argv[2])), ThrottledHandler)
print("ready", flush=True)
srv.serve_forever()
)PY";

// Raw-socket server with deliberately broken HTTP behavior, one mode per
// instantiation: each HttpCache error arc (truncated headers, garbage
// status line, redirects, chunked encoding, oversized headers, mid-body
// disconnects, and range-fetch contract violations) gets its own
// deterministic fixture. The fetch* modes answer the first "bytes=0-0"
// probe correctly (so the cache constructor succeeds) and sabotage every
// later range request. TOTAL (argv[3]) is the advertised source size.
constexpr const char* kMisbehavingServerScript = R"PY(
import re, socket, sys

MODE = sys.argv[1]
PORT = int(sys.argv[2])
TOTAL = int(sys.argv[3]) if len(sys.argv) > 3 else 0
fetch_count = 0  # block-fetch requests seen so far (probe excluded)

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", PORT))
srv.listen(8)
print("ready", flush=True)

def read_request(c):
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = c.recv(4096)
        if not chunk:
            break
        buf += chunk
    return buf

def ok_probe(total):
    return (b"HTTP/1.1 206 Partial Content\r\n"
            b"Content-Range: bytes 0-0/" + str(total).encode() + b"\r\n"
            b"Accept-Ranges: bytes\r\n"
            b"Content-Length: 1\r\n\r\nA")

def answer(c, req):
    global fetch_count
    if MODE == "close_early":
        return  # the caller closes without ever answering
    if MODE == "garbage":
        c.sendall(b"NOT-HTTP-AT-ALL\r\n\r\n")
        return
    if MODE == "redirect":
        c.sendall(b"HTTP/1.1 302 Found\r\nLocation: http://example.invalid/x\r\n"
                  b"Content-Length: 0\r\n\r\n")
        return
    if MODE == "chunked":
        # The gzip header exercises the "TE present but not chunked" arm of
        # the chunked detector; the second one trips the rejection.
        c.sendall(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n"
                  b"Transfer-Encoding: chunked\r\n\r\n")
        return
    if MODE == "status100":
        c.sendall(b"HTTP/1.1 100 Continue\r\n\r\n")
        return
    if MODE == "notfound":
        c.sendall(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n")
        return
    if MODE == "bigheaders":
        c.sendall(b"HTTP/1.1 200 OK\r\nX-Pad: " + b"a" * 70000)  # never terminated
        return
    if MODE == "truncated":
        c.sendall(b"HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 0-0/"
                  + str(TOTAL).encode() + b"\r\nContent-Length: 1\r\n\r\n")
        return  # the promised body byte never comes
    if MODE == "probe206norange":
        c.sendall(b"HTTP/1.1 206 Partial Content\r\nContent-Length: 1\r\n\r\nA")
        return
    # fetch* modes: the probe (bytes=0-0) must succeed so the cache
    # constructor goes online; every other range request is sabotaged.
    m = re.search(rb"Range:\s*bytes=(\d+)-(\d+)", req)
    if m and int(m.group(1)) == 0 and int(m.group(2)) == 0:
        c.sendall(ok_probe(TOTAL))
        return
    if m is None:
        return
    fetch_count += 1  # a non-probe range request just arrived
    if MODE == "fetchmalrange":
        # Cycle through every malformed Content-Range form the parser
        # rejects: all-whitespace, wrong unit, unparseable numbers, and
        # both parseable-but-wrong separators (dash checked first, so the
        # slash arm needs a valid dash).
        bad = [b"   ", b"items 1-2/3", b"bytes x-y/z", b"bytes 1x2y3",
               b"bytes 1-2z3"]
        cr = bad[min(fetch_count - 1, len(bad) - 1)]
        c.sendall(b"HTTP/1.1 206 Partial Content\r\nContent-Range:" + cr
                  + b"\r\nContent-Length: 1\r\n\r\nA")
        return
    first, last = int(m.group(1)), int(m.group(2))
    n = last - first + 1
    if MODE == "fetch200":
        c.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 256\r\n\r\n" + b"B" * 256)
    elif MODE == "fetchwrongrange":
        c.sendall(ok_probe(TOTAL))  # bytes 0-0 again, whatever was asked
    elif MODE == "fetchwrongtotal":
        c.sendall(b"HTTP/1.1 206 Partial Content\r\nContent-Range: bytes "
                  + str(first).encode() + b"-" + str(last).encode()
                  + b"/" + str(TOTAL + 1).encode()
                  + b"\r\nContent-Length: " + str(n).encode() + b"\r\n\r\n"
                  + b"C" * n)
    elif MODE == "fetchshort":
        half = n // 2
        c.sendall(b"HTTP/1.1 206 Partial Content\r\nContent-Range: bytes "
                  + str(first).encode() + b"-" + str(last).encode()
                  + b"/" + str(TOTAL).encode()
                  + b"\r\nContent-Length: " + str(half).encode() + b"\r\n\r\n"
                  + b"D" * half)
    elif MODE == "fetchnorange":
        c.sendall(b"HTTP/1.1 206 Partial Content\r\nContent-Length: "
                  + str(n).encode() + b"\r\n\r\n" + b"E" * n)

while True:
    try:
        c, _ = srv.accept()
    except OSError:
        break
    try:
        if MODE == "close_early":
            c.close()
            continue
        req = read_request(c)
        if req:
            answer(c, req)
    except OSError:
        pass  # the client hung up mid-answer (e.g. after the 64 KiB bail-out)
    finally:
        c.close()
)PY";

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

// Forks the shared throttled server on `port_base + getpid() % 200`.
// Same RAII contract as RangeServer: the child is always reaped, so a
// REQUIRE failure cannot leave it holding the harness's stdout pipe.
inline RangeServer startThrottledServer(const std::string& media_path,
                                        int port_base) {
  RangeServer srv;
  srv.port = port_base + (::getpid() % 200);
  std::string port_str = std::to_string(srv.port);
  char* const argv[] = {
    const_cast<char*>("python3"),
    const_cast<char*>("-c"),
    const_cast<char*>(kThrottledServerScript),
    const_cast<char*>(media_path.c_str()),
    const_cast<char*>(port_str.c_str()),
    nullptr,
  };
  srv.pid = startPipedServer(argv);
  srv.base_url = "http://127.0.0.1:" + std::to_string(srv.port);
  return srv;
}

// Same range server bound to ::1: lets the cache prove it can connect to a
// bracketed IPv6 literal url (getaddrinfo gets the bare "::1", the Host
// header keeps "[::1]:port"). pid < 0 means the host has no IPv6 stack —
// callers must treat that as a skip, not a failure.
inline RangeServer startRangeServerV6(const std::string& root_dir,
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
    const_cast<char*>("v6"),
    nullptr,
  };
  srv.pid = startPipedServer(argv);
  srv.base_url = "http://[::1]:" + std::to_string(srv.port);
  return srv;
}

// Forks the shared misbehaving-raw-server script in `mode` (see the script
// comment for the mode list). `total` is the advertised source size for the
// modes that need a good probe; pass 0 otherwise. Same RAII contract.
inline RangeServer startRawServer(const std::string& mode, int port_base,
                                  int total) {
  RangeServer srv;
  srv.port = port_base + (::getpid() % 200);
  std::string port_str = std::to_string(srv.port);
  std::string total_str = std::to_string(total);
  char* const argv[] = {
    const_cast<char*>("python3"),
    const_cast<char*>("-c"),
    const_cast<char*>(kMisbehavingServerScript),
    const_cast<char*>(mode.c_str()),
    const_cast<char*>(port_str.c_str()),
    const_cast<char*>(total_str.c_str()),
    nullptr,
  };
  srv.pid = startPipedServer(argv);
  srv.base_url = "http://127.0.0.1:" + std::to_string(srv.port);
  return srv;
}
#endif  // !_WIN32

}  // namespace test_servers

#endif  // SOAR_TEST_HTTP_SERVERS_H_
