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
import re, socket, sys, time

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
    if MODE == "stallbody":
        # Headers without Content-Length, three body bytes, then the
        # connection held open: the read-to-EOF path must end on its
        # receive timeout (a clean close mid-body is the sibling arm).
        c.sendall(b"HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n" + b"1\n0")
        time.sleep(30)
        return
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

// Subtitle-catalog fixture for the ExternalSubtitleProvider tests: a local
// stand-in for the remote search/fetch service the provider speaks to.
//   /search  requires the query parameters the wire protocol promises
//            (size=<digits>, hash=<16 lowercase hex>, name=<non-empty>) —
//            a 200 answer therefore proves the client sent a well-formed
//            query — and returns ROOT/catalog.tsv verbatim (comments,
//            tab-separated candidate lines and all).
//   /dl/<f>  serves ROOT/dl/<f> (root-escape and missing files are 404).
// Modes: "catalog" (normal), "empty" (200, empty body = no candidates),
// "status500", "status404", "stall" (accept, then sleep 40s before
// answering — the client's short socket timeout is what must save it),
// "nolength" (200 with no Content-Length and Connection: close — the
// read-until-close body path), "biglen" (a Content-Length far past the
// client's download cap, no body — the pre-read cap check), "big" (9 MiB
// streamed with no length — the streaming cap), "shortbody" (a /dl answer
// promising 100 bytes and delivering 3, then closing — the mid-body
// disconnect).
// APIKEY (argv[4], may be empty): when set, requests without the matching
// X-API-Key header are answered 401.
constexpr const char* kSubtitleServerScript = R"PY(
import os, re, sys, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

ROOT = os.path.abspath(sys.argv[1])
MODE = sys.argv[3]
APIKEY = sys.argv[4]

class SubtitleHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *args):
        pass
    def deny(self, code):
        self.send_response(code)
        self.send_header("Content-Length", "0")
        self.end_headers()
    def do_GET(self):
        if APIKEY and self.headers.get("X-API-Key") != APIKEY:
            self.deny(401)
            return
        url = urlparse(self.path)
        if url.path == "/search":
            q = parse_qs(url.query)
            ok = (q.get("size") and re.fullmatch(r"\d+", q["size"][0]) and
                  q.get("hash") and re.fullmatch(r"[0-9a-f]{16}", q["hash"][0]) and
                  q.get("name") and q["name"][0])
            if not ok:
                self.deny(400)
                return
            if MODE == "stall":
                time.sleep(40)
            if MODE == "status500":
                self.deny(500)
                return
            if MODE == "status404":
                self.deny(404)
                return
            if MODE == "biglen":
                # Advertise 9 MiB, send nothing: the client's cap check must
                # fire before the first body byte.
                self.send_response(200)
                self.send_header("Content-Length", str(9 * 1024 * 1024))
                self.end_headers()
                return
            data = b""
            if MODE in ("catalog", "nolength"):
                with open(os.path.join(ROOT, "catalog.tsv"), "rb") as f:
                    data = f.read()
            elif MODE == "big":
                data = b"\0" * (9 * 1024 * 1024)
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            if MODE in ("nolength", "big"):
                # HTTP/1.1 without Content-Length: the client must read to
                # EOF, so the connection has to actually close.
                self.send_header("Connection", "close")
                self.close_connection = True
            else:
                self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        if url.path.startswith("/dl/"):
            if MODE == "biglen":
                self.send_response(200)
                self.send_header("Content-Length", str(9 * 1024 * 1024))
                self.end_headers()
                return
            if MODE == "shortbody":
                # Promise 100 bytes, deliver 3, hang up: a clean close
                # mid-body, which the client must report as a failed fetch.
                self.send_response(200)
                self.send_header("Content-Length", "100")
                self.end_headers()
                self.wfile.write(b"1\n0")
                return
            target = os.path.normpath(os.path.join(ROOT, url.path.lstrip("/")))
            if not target.startswith(ROOT + os.sep) or not os.path.isfile(target):
                self.deny(404)
                return
            with open(target, "rb") as f:
                data = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            if MODE == "nolength":
                self.send_header("Connection", "close")
                self.close_connection = True
            else:
                self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        self.deny(404)

srv = ThreadingHTTPServer(("127.0.0.1", int(sys.argv[2])), SubtitleHandler)
print("ready", flush=True)
srv.serve_forever()
)PY";

// An OpenAI-compatible chat-completions fixture for the SubtitleTranslator
// tests. Every POST is recorded to <root>/requests.log as one tab-separated
// line — path, Content-Type, Authorization, body — so a test can assert the
// exact request shape the client produced; the body is a single line (the
// client's JSON writer escapes newlines), which is what makes the log
// line-oriented.
// MODE (argv[3]):
//   "ok" (default) answers the documented shape: it strips the leading
//       "N." numbering from each user-message line and replies with
//       numbered "[tr] <text>" lines — a deterministic round trip the test
//       can predict. The response is JSON with ensure_ascii=False (raw
//       UTF-8 in "content").
//   "asciiesc" answers the same content but serialized with
//       ensure_ascii=True, so non-ASCII comes back as \uXXXX escapes
//       (including surrogate pairs and a lone surrogate) — the client's
//       escape decoder must reconstruct the same text.
//   "status500" / "badjson" (an HTML body) / "nocontent" (choices[0] has
//       a message but no content) / "emptychoices" (choices: []) /
//       "shortlines" (the last numbered line is dropped) / "badnum" (the
//       lines lose their numbering) / "badsep" (a number with no
//       separator) / "onlydigits" (a bare number) / "status100" (an
//       informational status, which is not a final answer) exercise the
//       client's answer checks.
//   "sepvar" answers with the other documented separators — "1)" and
//       "2:" — a successful round trip the test can predict.
//   "shortbody" promises 100 bytes, delivers 3 and hangs up (the mid-body
//       disconnect); "stall" accepts and sleeps 40s before answering (the
//       socket timeout must end the wait).
//   Hand-built-body modes (json.dumps would re-escape the backslashes
//       these exist to put on the wire): "hexmix" (tab after the numbering
//       separator, uppercase hex, a 3-byte code point), "escmix" (the full
//       \/ \b \f \r \t simple-escape vocabulary), "neglen" (a negative
//       Content-Length: read to EOF instead), "contentnum" (content is a
//       JSON number, not a string), "badhex" (\u then non-hex),
//       "badescape" (\q — an undefined escape), "shortu" (\u truncated by
//       the end of the body), "trailbs" (a lone trailing backslash),
//       "unterm" (a string literal that never closes), "garbage" (not
//       HTTP at all — no status line).
// APIKEY (argv[4], may be empty): when set, requests whose Authorization
// header is not exactly "Bearer <key>" are answered 401 — after being
// recorded, so a test can see what leaked out.
constexpr const char* kChatServerScript = R"PY(
import json, os, re, sys, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.abspath(sys.argv[1])
MODE = sys.argv[3]
APIKEY = sys.argv[4]
NUM = re.compile(r"^\s*\d+[.)]:?\s*")

class ChatHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *args):
        pass
    def deny(self, code):
        self.send_response(code)
        self.send_header("Content-Length", "0")
        self.end_headers()
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(n).decode("utf-8", "replace")
        with open(os.path.join(ROOT, "requests.log"), "a", encoding="utf-8") as f:
            f.write("\t".join([self.path, self.headers.get("Content-Type", ""),
                               self.headers.get("Authorization", ""), body]) + "\n")
        if APIKEY and self.headers.get("Authorization") != "Bearer " + APIKEY:
            self.deny(401)
            return
        if MODE == "stall":
            time.sleep(40)
        if MODE == "status500":
            self.deny(500)
            return
        if MODE == "status100":
            # An informational status is not a final answer: only 2xx is.
            self.send_response(100)
            self.end_headers()
            return
        if MODE == "biglen":
            # Advertise a 9 MiB reply, send nothing: the client's cap check
            # must fire before the first body byte.
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(9 * 1024 * 1024))
            self.end_headers()
            return
        if MODE == "garbage":
            # Not HTTP at all: the first token carries no space, so there
            # is no status line to parse.
            self.wfile.write(b"NOT-HTTP-AT-ALL\r\n\r\n")
            self.close_connection = True
            return
        # Hand-built reply bodies: json.dumps would re-escape the
        # backslashes, and these modes exist precisely to put single-
        # backslash escape sequences on the wire for the client's JSON
        # string reader to chew on.
        raw = None
        if MODE == "hexmix":
            # Tab directly after the numbering separator, uppercase hex,
            # and a 3-byte code point — the reply parser and the escape
            # decoder must take all three.
            raw = b'{"choices":[{"message":{"content":"1.\\t\\u0041\\u00E9\\u4F60"}}]}'
        elif MODE == "escmix":
            # The full simple-escape vocabulary: \/ \b \f \r \t.
            raw = b'{"choices":[{"message":{"content":"1. a\\/b\\bc\\fc\\rd\\te"}}]}'
        elif MODE == "neglen":
            # A negative Content-Length is not a length: the client must
            # fall back to reading to EOF (and succeed).
            raw = b'{"choices":[{"message":{"content":"1. [tr] Solo"}}]}'
        elif MODE == "contentnum":
            # "content" that is not a JSON string at all.
            raw = b'{"choices":[{"message":{"content":123}}]}'
        elif MODE == "badhex":
            # \u followed by non-hex digits.
            raw = b'{"choices":[{"message":{"content":"1. x\\uZZZZ"}}]}'
        elif MODE == "badescape":
            # An escape the JSON vocabulary does not define.
            raw = b'{"choices":[{"message":{"content":"1. x\\qy"}}]}'
        elif MODE == "shortu":
            # The \u escape truncated by the end of the body: hex4 runs
            # off the string.
            raw = b'{"choices":[{"message":{"content":"1. x\\u00'
        elif MODE == "trailbs":
            # A lone trailing backslash where an escape should be.
            raw = b'{"choices":[{"message":{"content":"1. x\\'
        elif MODE == "unterm":
            # A string literal that never closes.
            raw = b'{"choices":[{"message":{"content":"1. x'
        if raw is not None:
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            if MODE == "neglen":
                self.send_header("Content-Length", "-1")
                self.send_header("Connection", "close")
                self.close_connection = True
            else:
                self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)
            return
        if MODE == "nolength":
            # A 200 with no Content-Length and Connection: close — the
            # read-until-EOF body path.
            data = json.dumps({"choices": [{"message": {"role": "assistant",
                                                        "content": "1. [tr] Solo"}}]},
                              ensure_ascii=True).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Connection", "close")
            self.close_connection = True
            self.end_headers()
            self.wfile.write(data)
            return
        if MODE == "shortbody":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", "100")
            self.end_headers()
            self.wfile.write(b"{\"a")
            return
        if MODE == "badjson":
            data = b"<html>oops</html>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        try:
            req = json.loads(body)
            text = req["messages"][-1]["content"]
            model = req.get("model", "")
        except Exception:
            self.deny(400)
            return
        lines = [NUM.sub("", l.strip()) for l in text.split("\n")]
        lines = [l for l in lines if l]
        if MODE == "shortlines" and lines:
            lines = lines[:-1]
        if MODE == "badnum":
            # Both ways a numbered reply can lose its shape: a line with
            # no number at all, and a number with no separator.
            content = "not numbered at all\n1x also not a separator"
        elif MODE == "badsep":
            # A number with no separator after it: the reply looks
            # numbered until the character where "." / ")" / ":" belongs.
            content = "1x also not a separator"
        elif MODE == "onlydigits":
            # Leading whitespace, then a bare number with nothing after
            # it: digits where a separator should be.
            content = "  12"
        elif MODE == "sepvar":
            # The documented reply accepts three separators: "12.",
            # "12)" and "12:".
            content = "1) [tr] Alpha\n2: [tr] Beta"
        elif MODE == "lonesur":
            # A canned single-line reply carrying a high surrogate followed
            # by a non-surrogate \uXXXX escape, and a lone low surrogate:
            # the client must decode both as U+FFFD without producing
            # invalid UTF-8.
            content = "1. [tr] A\ud800éB\udfffC"
        else:
            content = "\n".join("%d. [tr] %s" % (i + 1, l) for i, l in enumerate(lines))
        if MODE == "nocontent":
            payload = {"choices": [{"index": 0, "finish_reason": "stop",
                                    "message": {"role": "assistant"}}]}
        elif MODE == "emptychoices":
            payload = {"choices": []}
        else:
            payload = {"id": "chatcmpl-test", "object": "chat.completion",
                       "model": model,
                       "choices": [{"index": 0, "finish_reason": "stop",
                                    "message": {"role": "assistant",
                                                "content": content}}],
                       "usage": {"prompt_tokens": 1, "completion_tokens": 1,
                                 "total_tokens": 2}}
        ascii_safe = MODE in ("asciiesc", "lonesur")
        data = json.dumps(payload, ensure_ascii=ascii_safe).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

srv = ThreadingHTTPServer(("127.0.0.1", int(sys.argv[2])), ChatHandler)
print("ready", flush=True)
srv.serve_forever()
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

// Forks the shared subtitle-catalog server (kSubtitleServerScript) in
// `mode` with `api_key` as the expected X-API-Key ("" = no key check).
// Port base 24000+: the ranges below belong to the other suites (media
// 15k, CLI 15k/18.2k-18.5k, http_cache 18.6k-18.7k/24.56k). Same RAII
// contract as RangeServer.
inline RangeServer startSubtitleServer(const std::string& root_dir,
                                       const std::string& mode,
                                       const std::string& api_key) {
  RangeServer srv;
  srv.port = 24000 + (::getpid() % 200);
  std::string port_str = std::to_string(srv.port);
  char* const argv[] = {
    const_cast<char*>("python3"),
    const_cast<char*>("-c"),
    const_cast<char*>(kSubtitleServerScript),
    const_cast<char*>(root_dir.c_str()),
    const_cast<char*>(port_str.c_str()),
    const_cast<char*>(mode.c_str()),
    const_cast<char*>(api_key.c_str()),
    nullptr,
  };
  srv.pid = startPipedServer(argv);
  srv.base_url = "http://127.0.0.1:" + std::to_string(srv.port);
  return srv;
}

// Forks the chat-completions fixture server (kChatServerScript) in `mode`
// with `api_key` as the expected Bearer credential ("" = no key check).
// Port base 27000+: clear of the 24000s the catalog server uses — the two
// may live in the same test process. Same RAII contract as RangeServer.
inline RangeServer startChatServer(const std::string& root_dir,
                                   const std::string& mode,
                                   const std::string& api_key) {
  RangeServer srv;
  srv.port = 27000 + (::getpid() % 200);
  std::string port_str = std::to_string(srv.port);
  char* const argv[] = {
    const_cast<char*>("python3"),
    const_cast<char*>("-c"),
    const_cast<char*>(kChatServerScript),
    const_cast<char*>(root_dir.c_str()),
    const_cast<char*>(port_str.c_str()),
    const_cast<char*>(mode.c_str()),
    const_cast<char*>(api_key.c_str()),
    nullptr,
  };
  srv.pid = startPipedServer(argv);
  srv.base_url = "http://127.0.0.1:" + std::to_string(srv.port);
  return srv;
}
#endif  // !_WIN32

}  // namespace test_servers

#endif  // SOAR_TEST_HTTP_SERVERS_H_
