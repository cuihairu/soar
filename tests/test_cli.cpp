// Integration tests for the soar CLI (src/app/main.cpp). Each case runs
// the real executable as a subprocess and asserts the exit code and the
// combined stdout/stderr report. The SDL window block is reached three
// deterministic ways: the dummy video driver drives the renderer-failure
// path on every platform, and — where SOAR_TEST_X11 opts in (the CI
// coverage job) — a real Xvfb server plays the render loop to a clean
// exit, either through an XTEST Escape injection or through a
// WM_DELETE_WINDOW client message, which SDL turns into SDL_QUIT even
// with no window manager present. A third X11 case drives the ImGui HUD
// with a scripted XTEST session (click, wheel, key tour) so the
// player_window input branches land in the same run instead of being
// written off as untestable.
//
// SOAR_CLI_EXECUTABLE is passed in by tests/CMakeLists.txt as the path to
// the freshly built soar binary.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  define SOAR_POPEN _popen
#  define SOAR_PCLOSE _pclose
#else
#  include <dirent.h>
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <signal.h>
#  include <sys/socket.h>
#  include <sys/stat.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  define SOAR_POPEN popen
#  define SOAR_PCLOSE pclose
#endif

#include "test_http_servers.h"

#ifndef SOAR_CLI_EXECUTABLE
#  define SOAR_CLI_EXECUTABLE "soar"
#endif

namespace {

struct RunResult {
  int exit_code = -1;
  std::string output;
};

// Sets an environment variable for the duration of the scope and restores
// the previous value (or absence) afterwards. Doctest runs cases serially,
// but a leaked SDL driver or DISPLAY override would poison any later
// case's subprocess.
class ScopedEnv {
public:
  ScopedEnv(const char* name, const char* value) : name_(name) {
    const char* old = std::getenv(name);
    had_ = old != nullptr;
    if (had_) {
      old_ = old;
    }
#ifdef _WIN32
    _putenv_s(name_, value);
#else
    ::setenv(name_, value, /*overwrite=*/1);
#endif
  }
  ~ScopedEnv() {
#ifdef _WIN32
    if (had_) {
      _putenv_s(name_, old_.c_str());
    } else {
      _putenv_s(name_, "");
    }
#else
    if (had_) {
      ::setenv(name_, old_.c_str(), 1);
    } else {
      ::unsetenv(name_);
    }
#endif
  }
  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
  const char* name_;
  bool had_ = false;
  std::string old_;
};

bool envMediaPath(const char* name, std::string& out_path) {
  const char* path = std::getenv(name);
  if (!path || !*path) {
    return false;
  }
  std::ifstream f(path);
  if (!f.good()) {
    return false;
  }
  out_path = path;
  return true;
}

// One-shot injector for the X11 window tests. Shared preamble: wait for
// the soar window to map on the private X server. Then one of two exit
// deliveries, picked by argv[3]:
//
// "escape" — focus the window explicitly (no window manager runs under
//   Xvfb, so nothing owns input focus by default) and deliver a genuine
//   Escape through the XTEST extension until the window disappears (the
//   CLI exits cleanly on Escape). Decoding throttles to real time, so
//   this mode waits out the resolution changes of the multi-res fixture
//   before pressing anything, letting the render loop exercise its
//   texture re-creation branch. Coverage builds decode slower than the
//   wall clock (-O0 instrumentation), so the wait must be measured in
//   decode time, not decode speed assumptions: 4.5s crosses both
//   boundaries even when the decoder runs at half the presentation pace.
//
// "delete" — send a WM_DELETE_WINDOW client message. The protocol reply
//   needs no window manager: SDL itself listens for WM_PROTOCOLS and
//   converts the message into SDL_QUIT, so this drives the QUIT branch
//   of the event loop from outside the process.
//
// "drive" — a scripted UI session: a key tour through every shortcut
//   family (overlays, fullscreen, volume, mute, track cycling, seeks,
//   percent jump, pause/resume), a click tour (hide OSC, show it again,
//   double-click fullscreen, Escape back), the wheel family (vertical
//   volume both ways, Shift+wheel seek, horizontal seek), and a
//   press-drag-release over the seek bar (scrub preview + commit), then
//   q until the window disappears. This is what exercises the ImGui
//   HUD's input branches (docs/ui-design.md §3) end to end.
//
// In every mode the injector exits 0 once the window disappears (the CLI
// exited); a non-zero code means it could not do its job.
const char* const kX11InjectorScript = R"PY(import sys, time

from Xlib import X, display
from Xlib.ext import xtest
from Xlib.protocol import event

disp_name, title, mode = sys.argv[1], sys.argv[2], sys.argv[3]
d = display.Display(disp_name)


def find_window():
    try:
        for child in d.screen().root.query_tree().children:
            try:
                if child.get_wm_name() == title:
                    return child
            except Exception:
                pass
    except Exception:
        pass
    return None


win = None
deadline = time.monotonic() + 15.0
while win is None and time.monotonic() < deadline:
    win = find_window()
    if win is None:
        time.sleep(0.1)
if win is None:
    sys.exit(3)

if mode == "delete":
    wm_protocols = d.intern_atom("WM_PROTOCOLS")
    wm_delete = d.intern_atom("WM_DELETE_WINDOW")
    msg = event.ClientMessage(
        window=win.id,
        client_type=wm_protocols,
        data=(32, [wm_delete, X.CurrentTime, 0, 0, 0]),
        format=32,
    )
    win.send_event(msg, event_mask=X.NoEventMask)
    d.sync()
    deadline = time.monotonic() + 15.0
    while time.monotonic() < deadline:
        if find_window() is None:
            sys.exit(0)  # the window died: the CLI exited
        time.sleep(0.2)
    sys.exit(5)

if mode == "drive":
    # Let the UI come up and the null backend start before touching it.
    time.sleep(1.5)

    def focus():
        win.set_input_focus(X.RevertToParent, X.CurrentTime)
        d.sync()

    def key(kc):
        xtest.fake_input(d, X.KeyPress, kc)
        d.sync()
        xtest.fake_input(d, X.KeyRelease, kc)
        d.sync()

    def button(btn):
        xtest.fake_input(d, X.ButtonPress, btn)
        d.sync()
        xtest.fake_input(d, X.ButtonRelease, btn)
        d.sync()

    def moved(x, y):
        # python-xlib: warp_pointer(x, y, ...) — dest is relative to the
        # window; no source-rectangle filtering needed.
        win.warp_pointer(x, y)
        d.sync()

    try:
        geo = win.get_geometry()
        cx, cy = geo.width // 2, geo.height // 2
        # Key tour: overlays open+close, fullscreen round-trip, volume and
        # mute, track cycling (subtitles on then off), the seek family
        # (Home/Left/Right/PageUp/PageDown/50%), pause/resume, an
        # end-of-media round trip (90% + PageDown lands exactly on the
        # 10-minute mark -> Ended; space replays), rate down/up.
        tour = (0x69, 0x69, 0x72, 0x72, 0x68, 0x68,
                0x66, 0xFF1B,
                0xFF52, 0xFF54, 0x6D, 0x6D,
                0x61, 0x63, 0x63,
                0xFF50, 0xFF51, 0xFF53, 0xFF55, 0xFF56, 0x35,
                0x20, 0x20,
                0x39, 0xFF56,
                0x20,
                0x2C, 0x2C, 0x2E, 0x2E)
        for keysym in tour:
            focus()
            key(d.keysym_to_keycode(keysym))
            time.sleep(0.25)
        # Click tour at the video area (no widget there): hide the OSC,
        # bring it back, then a fast pair for the double-click fullscreen
        # (and Escape back out of it).
        moved(cx, cy)
        time.sleep(0.3)
        button(1); time.sleep(0.7)  # visible -> force-hide
        button(1); time.sleep(0.3)  # hidden -> show + fresh idle window
        button(1); time.sleep(0.25)  # pair member one
        button(1); time.sleep(0.4)   # within 500ms: double click -> fullscreen
        focus(); key(d.keysym_to_keycode(0xFF1B)); time.sleep(0.4)
        # Wheel: vertical volume both ways, Shift+wheel seek, button 6 as
        # horizontal seek where the server maps it.
        button(4); time.sleep(0.25)
        button(5); time.sleep(0.25)
        sh = d.keysym_to_keycode(0xFFE1)  # Shift_L
        xtest.fake_input(d, X.KeyPress, sh); d.sync()
        button(4)
        xtest.fake_input(d, X.KeyRelease, sh); d.sync()
        time.sleep(0.25)
        button(6); time.sleep(0.25)
        # Widget phase over the OSC row (window coordinates; the bitmap
        # font makes these stable across machines). Buttons first, then a
        # volume-slider drag, then the combos — each opened, one item
        # picked. A key press while a popup is open exercises the app
        # shortcut suppression.
        def wclick(x, y):
            moved(x, y); time.sleep(0.15)
            button(1); time.sleep(0.3)

        wclick(59, 495)    # Pause
        wclick(59, 495)    # resume
        wclick(117, 495)   # Stop
        wclick(59, 495)    # play again from Stopped
        wclick(269, 495)   # Mute (label widens to "Unmute" and the volume
                           # slider shifts right — the drag below stays in
                           # the intersection of both label variants)
        moved(320, 495); time.sleep(0.15)
        xtest.fake_input(d, X.ButtonPress, 1); d.sync(); time.sleep(0.1)
        moved(350, 495); time.sleep(0.1)
        xtest.fake_input(d, X.ButtonRelease, 1); d.sync(); time.sleep(0.3)
        wclick(399, 495)   # rate combo open (popup flips up: no room below)
        focus(); key(d.keysym_to_keycode(0x69)); time.sleep(0.2)  # suppressed
        wclick(399, 468)   # pick 2x (bottom row, clear of the 1.0x default)
        wclick(477, 495)   # audio combo open (single item fits below)
        wclick(506, 522)   # pick the audio track
        wclick(569, 495)   # subtitle combo open (flips up)
        wclick(591, 468)   # pick the subtitle track ...
        wclick(569, 495)   # ... reopen; "Off" only acts as a change when a
        wclick(591, 443)   # track is currently selected, so the order matters
        wclick(641, 495)   # Info overlay
        wclick(641, 495)   # (close again)
        wclick(693, 495)   # fullscreen toggle button
        focus(); key(d.keysym_to_keycode(0xFF1B)); time.sleep(0.3)
        # Seek bar: the row-1 slider sits at y 452..477 under the OSC top
        # edge. A press-drag-back-release first (the value returns to where
        # it started, so the gesture is a cancelled drag that must not
        # seek), then a press-drag-release so the scrub preview and the
        # release-commit arcs both run.
        moved(cx - 80, geo.height - 76)
        time.sleep(0.2)
        xtest.fake_input(d, X.ButtonPress, 1); d.sync()
        time.sleep(0.1)
        moved(cx + 80, geo.height - 76); time.sleep(0.1)
        moved(cx - 80, geo.height - 76); time.sleep(0.1)
        xtest.fake_input(d, X.ButtonRelease, 1); d.sync()
        time.sleep(0.3)
        moved(cx - 80, geo.height - 76)
        time.sleep(0.3)
        xtest.fake_input(d, X.ButtonPress, 1); d.sync()
        time.sleep(0.1)
        moved(cx + 80, geo.height - 76)
        time.sleep(0.1)
        xtest.fake_input(d, X.ButtonRelease, 1); d.sync()
        time.sleep(0.3)
        # Recent overlay: the seeded list is [sample, noseek-live,
        # fail-open-x] with one 19px row per entry. Reopening the
        # unseekable source is the seek-shortcut degradation, the third
        # entry refuses to open at all (the deleted-file path), and the
        # list is reordered by every successful pick — so each click
        # below accounts for the move-to-front the previous one caused.
        focus(); key(d.keysym_to_keycode(0x72))
        time.sleep(1.2)
        moved(480, 158); time.sleep(0.15)   # row 2: asset://noseek-live
        button(1); time.sleep(0.9)
        focus(); key(d.keysym_to_keycode(0x20)); time.sleep(0.25)  # pause
        focus(); key(d.keysym_to_keycode(0xFF51)); time.sleep(0.25)  # Left
        focus(); key(d.keysym_to_keycode(0xFF50)); time.sleep(0.25)  # Home
        focus(); key(d.keysym_to_keycode(0x35)); time.sleep(0.25)   # '5' = 50%
        focus(); key(d.keysym_to_keycode(0x72))
        time.sleep(1.2)
        moved(480, 177); time.sleep(0.15)   # row 3: asset://fail-open-x
        button(1); time.sleep(0.9)
        focus(); key(d.keysym_to_keycode(0x69))  # Info shows the error row
        time.sleep(0.5)
        focus(); key(d.keysym_to_keycode(0x69)); time.sleep(0.3)
        focus(); key(d.keysym_to_keycode(0x72))
        time.sleep(1.2)
        moved(480, 158); time.sleep(0.15)   # row 2 again: back to the sample
        button(1); time.sleep(0.9)
        # Squeeze the window to nothing and back. Without a window manager
        # XResizeWindow applies directly, so SDL sees a 1x1 drawable: the
        # HUD must skip its draws instead of computing a negative OSC
        # width, and the next frame must draw normally again.
        win.configure(width=1, height=1)
        d.sync()
        time.sleep(0.6)
        win.configure(width=960, height=540)
        d.sync()
        time.sleep(0.6)
    except Exception:
        pass  # best effort: whatever ran before an error still counts
    qk = d.keysym_to_keycode(0x71)
    deadline = time.monotonic() + 60.0
    while time.monotonic() < deadline:
        try:
            focus()
            key(qk)
        except Exception:
            sys.exit(0)
        time.sleep(0.5)
        if find_window() is None:
            sys.exit(0)
    sys.exit(4)

if mode == "stall":
    # The window's buffering indicator is the one HUD element driven by a
    # backend event rather than by input: the UI polls the atomic that
    # main.cpp sets from BufferingStarted/Ended. Sit still long enough for
    # the throttled server's mid-body pause to cross the backend's 10s
    # stall threshold, then quit. No clicking: the HUD is allowed to
    # auto-hide, which is the point (the chip is not part of the OSC).
    time.sleep(4)
    # The fixture is audio-only, so the subtitle shortcuts have nothing to
    # cycle and the HUD has to say so instead of silently doing nothing —
    # the same toast a user gets pressing C on a movie without subtitles.
    try:
        win.set_input_focus(X.RevertToParent, X.CurrentTime)
        d.sync()
        c = d.keysym_to_keycode(0x63)
        xtest.fake_input(d, X.KeyPress, c)
        d.sync()
        xtest.fake_input(d, X.KeyRelease, c)
        d.sync()
    except Exception:
        pass
    time.sleep(16)
    qk = d.keysym_to_keycode(0x71)
    deadline = time.monotonic() + 30.0
    while time.monotonic() < deadline:
        try:
            win.set_input_focus(X.RevertToParent, X.CurrentTime)
            d.sync()
            xtest.fake_input(d, X.KeyPress, qk)
            d.sync()
            xtest.fake_input(d, X.KeyRelease, qk)
            d.sync()
        except Exception:
            sys.exit(0)
        if find_window() is None:
            sys.exit(0)
        time.sleep(0.5)
    sys.exit(4)

if mode == "download":
    # A --cache-dir download is paced by playback: this short fixture's
    # terminal 100% progress event lands near its 6s mark. Sit still past
    # that point, then quit, so the run's event stream holds the whole
    # download arc.
    time.sleep(8)
    qk = d.keysym_to_keycode(0x71)
    deadline = time.monotonic() + 30.0
    while time.monotonic() < deadline:
        try:
            win.set_input_focus(X.RevertToParent, X.CurrentTime)
            d.sync()
            xtest.fake_input(d, X.KeyPress, qk)
            d.sync()
            xtest.fake_input(d, X.KeyRelease, qk)
            d.sync()
        except Exception:
            sys.exit(0)
        if find_window() is None:
            sys.exit(0)
        time.sleep(0.5)
    sys.exit(4)

time.sleep(4.5)
esc = d.keysym_to_keycode(0xFF1B)  # XK_Escape
deadline = time.monotonic() + 60.0
while time.monotonic() < deadline:
    try:
        win.set_input_focus(X.RevertToParent, X.CurrentTime)
        d.sync()
        xtest.fake_input(d, X.KeyPress, esc)
        d.sync()
        xtest.fake_input(d, X.KeyRelease, esc)
        d.sync()
    except Exception:
        sys.exit(0)  # the window died: the CLI exited
    time.sleep(0.5)
    if find_window() is None:
        sys.exit(0)
sys.exit(4)
)PY";

RunResult runCli(const std::vector<std::string>& args) {
  // Windows _popen routes through `cmd.exe /C`, which (for lines with more
  // than two quotes) strips the first and the last quote character:
  // `"exe" "a" "b"` becomes `exe" "a" "b` and fails to run at all. Quoting
  // only the executable survives the strip in both cases — a path without
  // spaces loses both quotes (`exe a b`), a path with spaces keeps them
  // (two quotes, whitespace between, executable name). The arguments used
  // by these tests are simple tokens without spaces or quotes.
  std::string cmd(1, '"');
  cmd += SOAR_CLI_EXECUTABLE;
  cmd += '"';
  for (const auto& arg : args) {
    cmd += ' ';
#ifdef _WIN32
    cmd += arg;
#else
    cmd += '"';
    cmd += arg;
    cmd += '"';
#endif
  }
  cmd += " 2>&1"; // merge stderr into the captured stream
#ifdef __linux__
  // A sanitizer-instrumented child can hang indefinitely: observed once in
  // CI, where the tsan CLI stalled right after fetching an HLS master's
  // segments and burned the entire 300s CTest budget. Cap every run so a
  // hung child degrades into a normal failing assertion (exit 124) with
  // whatever output it produced, instead of a context-free suite timeout.
  // Linux-only: coreutils' timeout is guaranteed there; macOS ships no
  // such command (its homebrew build installs gtimeout instead) and
  // prefixing it made every child exit 127.
  cmd.insert(0, "timeout 90 ");
#endif

  RunResult result;
  FILE* pipe = SOAR_POPEN(cmd.c_str(), "r");
  REQUIRE(pipe != nullptr);
  char buffer[512];
  std::size_t n = 0;
  while ((n = fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
    result.output.append(buffer, n);
  }

#ifdef _WIN32
  result.exit_code = SOAR_PCLOSE(pipe);
#else
  const int status = SOAR_PCLOSE(pipe);
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
  return result;
}

// Shared by the adaptive-protocol (HLS/DASH) cases: fork a python3
// http.server rooted at dir, wait until it accepts connections, run the
// headless CLI against http://127.0.0.1:<port><url_path>, and reap the
// server. skipped marks the environment gaps (no python3 / server never
// came up) that callers turn into skip MESSAGEs; the fixture checks stay
// in the test cases so mac/win runners without fixtures skip first.
// POSIX-only (fork/execlp): the Windows branches of those cases skip and
// never reference it.
#ifndef _WIN32
struct HttpCliRun {
  RunResult cli;
  bool skipped = false;
};

HttpCliRun runHeadlessOverHttpDir(const std::string& dir, const std::string& url_path,
                                  int port_base) {
  HttpCliRun result;
  result.skipped = true; // until the server is up and the CLI has run
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    return result;
  }
  const std::string port = std::to_string(port_base + (::getpid() % 2000));

  const pid_t server = ::fork();
  REQUIRE(server >= 0);
  if (server == 0) {
    ::execlp("python3", "python3", "-m", "http.server", port.c_str(),
             "--bind", "127.0.0.1", "--directory", dir.c_str(),
             static_cast<char*>(nullptr));
    _exit(127);
  }

  // Wait until the server accepts connections before pointing the CLI at it.
  bool ready = false;
  for (int attempt = 0; attempt < 40 && !ready; ++attempt) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(static_cast<uint16_t>(std::stoi(port)));
      addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      ready = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
      ::close(fd);
    }
    if (!ready) {
      ::usleep(100 * 1000);
    }
  }
  if (!ready) {
    ::kill(server, SIGTERM);
    ::waitpid(server, nullptr, 0);
    return result;
  }

  result.cli = runCli({"--headless", "--backend=ffmpeg",
                       "http://127.0.0.1:" + port + url_path});
  result.skipped = false;
  ::kill(server, SIGTERM);
  ::waitpid(server, nullptr, 0);
  return result;
}

// Minimal RTSP/RTP server for the live-stream smoke case. FFmpeg used to be
// able to serve as the pushing side itself (`-rtsp_flags listen` on the
// muxer), but FFmpeg 8 dropped that option, and even where it still exists
// it would be version-dependent — so the fixture is a ~100-line python
// script instead: RFC 2326 basics (OPTIONS/DESCRIBE/SETUP/PLAY, CSeq
// echoed), a static SDP advertising PCMA (RTP payload type 8, no dynamic
// negotiation), and 8 s of 20 ms RTP frames of A-law silence pumped over
// UDP once PLAY is answered. Verified by hand against the FFmpeg 8 rtsp
// demuxer: it probes with OPTIONS, takes the SDP, negotiates client_port
// in SETUP and starts reading as soon as PLAY returns.
constexpr char kRtspServerScript[] = R"PY(
import re, socket, sys, time

port = int(sys.argv[1])
payload = bytes([0xD5]) * 160  # 20 ms of PCMA silence at 8 kHz

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", port))
srv.listen(1)
print("ready", flush=True)  # harness reads this line over a pipe, no TCP probe
conn, _ = srv.accept()
conn.settimeout(30)
udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
seq = 0
ts = 0
client_port = 0
streaming = False
buf = b""
try:
    while True:
        try:
            data = conn.recv(4096)
        except socket.timeout:
            break
        if not data:
            break
        buf += data
        while b"\r\n\r\n" in buf:
            raw, buf = buf.split(b"\r\n\r\n", 1)
            req = raw.decode("ascii", "replace")
            head = req.splitlines()[0] if req else ""
            cseq = "0"
            for line in req.splitlines()[1:]:
                if line.lower().startswith("cseq:"):
                    cseq = line.split(":", 1)[1].strip()
            ok = "RTSP/1.0 200 OK\r\nCSeq: " + cseq + "\r\n"
            if head.startswith("OPTIONS"):
                resp = ok + "Public: OPTIONS, DESCRIBE, SETUP, TEARDOWN, PLAY, PAUSE\r\n\r\n"
            elif head.startswith("DESCRIBE"):
                sdp = ("v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=soar-test\r\n"
                       "c=IN IP4 127.0.0.1\r\nt=0 0\r\n"
                       "m=audio 0 RTP/AVP 8\r\na=control:trackID=0\r\n")
                resp = (ok + "Content-Type: application/sdp\r\nContent-Length: "
                        + str(len(sdp)) + "\r\n\r\n" + sdp)
            elif head.startswith("SETUP"):
                m = re.search(r"client_port=(\d+)", req)
                client_port = int(m.group(1)) if m else 0
                tp = "RTP/AVP;unicast;client_port=%d-%d;server_port=30000-30001" % (
                    client_port, client_port + 1)
                resp = ok + "Session: 1\r\nTransport: " + tp + "\r\n\r\n"
            elif head.startswith("PLAY"):
                streaming = True
                resp = ok + "Session: 1\r\n\r\n"
            elif head.startswith("TEARDOWN"):
                resp = ok + "Session: 1\r\n\r\n"
            else:
                resp = ok + "\r\n"
            conn.sendall(resp.encode())
            if streaming:
                streaming = False
                for _ in range(400):  # ~8 s of audio, sent faster than realtime
                    hdr = (bytes([0x80, 8]) + seq.to_bytes(2, "big")
                           + (ts & 0xFFFFFFFF).to_bytes(4, "big") + b"SOTR")
                    udp.sendto(hdr + payload, ("127.0.0.1", client_port))
                    seq += 1
                    ts += 160
                    time.sleep(0.002)
except Exception:
    pass
)PY";

// Runs the headless CLI against a forked kRtspServerScript instance. Same
// shape as runHeadlessOverHttpDir (fork, wait for readiness, run, reap) but
// with no fixture directory: the server synthesizes its stream in memory.
// Readiness comes over a pipe instead of a TCP probe: the server accepts
// exactly one connection, so a probe connect would consume that accept and
// leave the CLI talking to a dead server (observed: probe OK, open failed).
HttpCliRun runHeadlessOverRtsp(int port_base) {
  HttpCliRun result;
  result.skipped = true; // until the server is up and the CLI has run
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    return result;
  }
  const std::string port = std::to_string(port_base + (::getpid() % 2000));

  int ready_pipe[2];
  REQUIRE(::pipe(ready_pipe) == 0);
  const pid_t server = ::fork();
  REQUIRE(server >= 0);
  if (server == 0) {
    ::close(ready_pipe[0]);
    ::dup2(ready_pipe[1], 1); // script prints "ready" on stdout
    ::close(ready_pipe[1]);
    ::execlp("python3", "python3", "-c", kRtspServerScript, port.c_str(),
             static_cast<char*>(nullptr));
    _exit(127);
  }
  ::close(ready_pipe[1]);

  // Read until the newline; EOF means the script died before listening
  // (e.g. bind failure) and the case must skip rather than hang.
  std::string ready_line;
  char ch = 0;
  while (::read(ready_pipe[0], &ch, 1) == 1) {
    ready_line += ch;
    if (ch == '\n') {
      break;
    }
  }
  ::close(ready_pipe[0]);
  if (ready_line.find("ready") == std::string::npos) {
    ::kill(server, SIGTERM);
    ::waitpid(server, nullptr, 0);
    return result;
  }

  result.cli = runCli({"--headless", "--backend=ffmpeg",
                       "rtsp://127.0.0.1:" + port + "/live"});
  result.skipped = false;
  ::kill(server, SIGTERM);
  ::waitpid(server, nullptr, 0);
  return result;
}
#endif // !_WIN32

} // namespace

TEST_CASE("no arguments prints usage and exits with 2") {
  const auto run = runCli({});
  CHECK(run.exit_code == 2);
  CHECK(run.output.find("Usage:") != std::string::npos);
}

TEST_CASE("unknown option prints usage and exits with 2") {
  const auto run = runCli({"--bogus", "asset://sample"});
  CHECK(run.exit_code == 2);
  CHECK(run.output.find("Unknown option: --bogus") != std::string::npos);
  CHECK(run.output.find("Usage:") != std::string::npos);
}

TEST_CASE("options without a source URI exit with 2") {
  const auto run = runCli({"--headless"});
  CHECK(run.exit_code == 2);
  CHECK(run.output.find("Usage:") != std::string::npos);
}

TEST_CASE("unknown backend name exits with 2") {
  const auto run = runCli({"--backend=walrus", "asset://sample"});
  CHECK(run.exit_code == 2);
  CHECK(run.output.find("Unknown backend type: walrus") != std::string::npos);
  CHECK(run.output.find("Available backends: null") != std::string::npos);
}

TEST_CASE("headless run over the null backend succeeds") {
  const auto run = runCli({"--headless", "--backend=null", "asset://sample"});
  CHECK(run.exit_code == 0);
  CHECK(run.output.find("Using null backend") != std::string::npos);
  // The headless flow announces the media and drives seek/pause/stop.
  CHECK(run.output.find("=== Media Info ===") != std::string::npos);
  CHECK(run.output.find("event: media-info") != std::string::npos);
}

TEST_CASE("headless run over an unopenable source fails with 1") {
  // "fail-open" makes the null backend simulate a failed open.
  const auto run = runCli({"--headless", "--backend=null", "asset://fail-open.mp4"});
  CHECK(run.exit_code == 1);
  CHECK(run.output.find("Failed to open source: asset://fail-open.mp4") != std::string::npos);
}

TEST_CASE("windowed run fails cleanly at SDL_Init") {
  // Rather than gambling on which environments refuse SDL video init
  // (a no-DISPLAY runner can still succeed through kmsdrm and hang in
  // the window loop forever), point SDL_VIDEODRIVER at a driver that
  // does not exist: SDL_Init must then fail on every platform, and the
  // run must report and exit 1 instead of crashing. The child inherits
  // this process's environment; doctest runs cases serially, so the
  // temporary variable cannot leak into a concurrent subprocess.
  const char* const bogus_driver = "soar-no-such-video-driver";
#ifdef _WIN32
  // _putenv("") removes the variable on Windows.
  _putenv("SDL_VIDEODRIVER=");
  _putenv_s("SDL_VIDEODRIVER", bogus_driver);
  const auto run = runCli({"--backend=null", "asset://sample"});
  _putenv("SDL_VIDEODRIVER=");
#else
  setenv("SDL_VIDEODRIVER", bogus_driver, /*overwrite=*/1);
  const auto run = runCli({"--backend=null", "asset://sample"});
  unsetenv("SDL_VIDEODRIVER");
#endif
  // A build without SDL2 never reaches the window block; the marker
  // check skips that case rather than asserting on unrelated output.
  if (run.output.find("SDL_Init failed") == std::string::npos) {
    MESSAGE("SDL2 not compiled in; window block absent; skipping");
    return;
  }
  CHECK(run.exit_code == 1);
}

TEST_CASE("renderer selection fails cleanly under the dummy video driver") {
  // The dummy video driver never provides an accelerated renderer, and
  // main.cpp requests SDL_RENDERER_ACCELERATED explicitly — so on every
  // platform this run reaches the renderer-failure branch and exits 1
  // cleanly. dummy audio keeps SDL_Init itself deterministic (a machine
  // without audio hardware would otherwise fail one step earlier).
  const ScopedEnv video("SDL_VIDEODRIVER", "dummy");
  const ScopedEnv audio("SDL_AUDIODRIVER", "dummy");
  const auto run = runCli({"--backend=null", "asset://sample"});
  if (run.output.find("SDL_CreateRenderer failed") == std::string::npos) {
    MESSAGE("SDL2 not compiled in; renderer block absent; skipping");
    return;
  }
  CHECK(run.exit_code == 1);
}

TEST_CASE("windowed run plays on a real X server and exits cleanly on Escape") {
#ifdef _WIN32
  MESSAGE("the X11 window test is POSIX-only; skipping");
  return;
#else
  // Opt-in (the CI coverage job): only environments that ship Xvfb and
  // python3-xlib should attempt window automation.
  if (std::getenv("SOAR_TEST_X11") == nullptr) {
    MESSAGE("SOAR_TEST_X11 not set; skipping the X11 window test");
    return;
  }
  std::string media;
  if (!envMediaPath("SOAR_TEST_MULTI_RES", media)) {
    MESSAGE("SOAR_TEST_MULTI_RES not set; skipping the X11 window test");
    return;
  }
  // Both tools are installed exactly where SOAR_TEST_X11 is set; the
  // explicit probe turns a misconfigured environment into a skip instead
  // of a subprocess that renders forever because no key ever arrives.
  if (std::system("command -v Xvfb >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1 && "
                  "python3 -c 'from Xlib.ext import xtest' >/dev/null 2>&1") != 0) {
    MESSAGE("Xvfb or python3-xlib missing; skipping the X11 window test");
    return;
  }

  // A private display per run: the pid offset keeps concurrent test
  // processes and stale servers from a previous run off this one.
  const std::string suffix = std::to_string(70 + (::getpid() % 25));
  const std::string display = ":" + suffix;
  const std::string socket = "/tmp/.X11-unix/X" + suffix;

  pid_t xvfb = ::fork();
  REQUIRE(xvfb >= 0);
  if (xvfb == 0) {
    ::execlp("Xvfb", "Xvfb", display.c_str(), "-screen", "0", "640x480x24",
             "-ac", "-nolisten", "tcp", static_cast<char*>(nullptr));
    _exit(127);
  }
  bool server_up = false;
  for (int i = 0; i < 50 && !server_up; ++i) {
    struct stat st;
    server_up = ::stat(socket.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
    if (!server_up) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  if (!server_up) {
    MESSAGE("Xvfb failed to start; skipping the X11 window test");
    ::kill(xvfb, SIGTERM);
    ::waitpid(xvfb, nullptr, 0);
    return;
  }

  // runCli below blocks on the CLI, so the injector runs as a sibling and
  // exits on its own once the window disappears.
  pid_t injector = ::fork();
  REQUIRE(injector >= 0);
  if (injector == 0) {
    ::execlp("python3", "python3", "-c", kX11InjectorScript, display.c_str(),
             "soar", "escape", static_cast<char*>(nullptr));
    _exit(127);
  }

  const ScopedEnv display_env("DISPLAY", display.c_str());
  // Same reason as the sanitizer jobs: keep libpulse out of the picture.
  const ScopedEnv audio_env("SDL_AUDIODRIVER", "dummy");
  const auto run = runCli({"--backend=ffmpeg", media});

  // The position/state events show how far real-time decode advanced
  // before the Escape exit — that progress is what decides whether the
  // texture re-creation branch runs, so keep it in the CI log.
  std::printf("x11 window test: cli exit=%d, output:\n%s\n", run.exit_code, run.output.c_str());

  ::waitpid(injector, nullptr, 0);
  ::kill(xvfb, SIGTERM);
  ::waitpid(xvfb, nullptr, 0);

  if (run.output.find("SDL_CreateRenderer failed") != std::string::npos) {
    // An X server without a usable GL stack cannot satisfy the accelerated
    // renderer request; that environment gap is a skip, not a failure.
    MESSAGE("no accelerated renderer under this X server; skipping");
    return;
  }
  CHECK(run.exit_code == 0);
  CHECK(run.output.find("=== Media Info ===") != std::string::npos);
#ifdef SOAR_WITH_FFMPEG
  CHECK(run.output.find("Using FFmpeg backend") != std::string::npos);
#else
  // A build without FFmpeg support still plays the loop on the null
  // backend; the video-frame block inside the loop simply compiles out.
  CHECK(run.output.find("Falling back to null backend") != std::string::npos);
#endif
#endif
}

TEST_CASE("windowed null-backend run exits cleanly on WM_DELETE_WINDOW") {
#ifdef _WIN32
  MESSAGE("the X11 window test is POSIX-only; skipping");
  return;
#else
  // Opt-in (the CI coverage job), same gating as the Escape test above.
  if (std::getenv("SOAR_TEST_X11") == nullptr) {
    MESSAGE("SOAR_TEST_X11 not set; skipping the X11 window test");
    return;
  }
  if (std::system("command -v Xvfb >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1 && "
                  "python3 -c 'from Xlib.ext import xtest' >/dev/null 2>&1") != 0) {
    MESSAGE("Xvfb or python3-xlib missing; skipping the X11 window test");
    return;
  }

  // The Escape test uses the same scheme; cases run serially and each
  // server is reaped below, so the display number can be reused.
  const std::string suffix = std::to_string(70 + (::getpid() % 25));
  const std::string display = ":" + suffix;
  const std::string socket = "/tmp/.X11-unix/X" + suffix;

  pid_t xvfb = ::fork();
  REQUIRE(xvfb >= 0);
  if (xvfb == 0) {
    ::execlp("Xvfb", "Xvfb", display.c_str(), "-screen", "0", "640x480x24",
             "-ac", "-nolisten", "tcp", static_cast<char*>(nullptr));
    _exit(127);
  }
  bool server_up = false;
  for (int i = 0; i < 50 && !server_up; ++i) {
    struct stat st;
    server_up = ::stat(socket.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
    if (!server_up) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  if (!server_up) {
    MESSAGE("Xvfb failed to start; skipping the X11 window test");
    ::kill(xvfb, SIGTERM);
    ::waitpid(xvfb, nullptr, 0);
    return;
  }

  pid_t injector = ::fork();
  REQUIRE(injector >= 0);
  if (injector == 0) {
    ::execlp("python3", "python3", "-c", kX11InjectorScript, display.c_str(),
             "soar", "delete", static_cast<char*>(nullptr));
    _exit(127);
  }

  const ScopedEnv display_env("DISPLAY", display.c_str());
  const ScopedEnv audio_env("SDL_AUDIODRIVER", "dummy");
  // The null backend keeps this case media-free: the QUIT event branch,
  // the ffmpeg-backend short-circuit guarding the frame block, and the
  // never-created-texture cleanup arc are all reachable without decoding.
  const auto run = runCli({"--backend=null", "asset://sample"});

  std::printf("x11 delete-window test: cli exit=%d, output:\n%s\n", run.exit_code, run.output.c_str());

  ::waitpid(injector, nullptr, 0);
  ::kill(xvfb, SIGTERM);
  ::waitpid(xvfb, nullptr, 0);

  if (run.output.find("SDL_CreateRenderer failed") != std::string::npos) {
    // Same environment gap as the Escape test: a skip, not a failure.
    MESSAGE("no accelerated renderer under this X server; skipping");
    return;
  }
  CHECK(run.exit_code == 0);
#endif
}

TEST_CASE("windowed UI run drives the HUD through a scripted XTEST session") {
#ifdef _WIN32
  MESSAGE("the X11 window test is POSIX-only; skipping");
  return;
#else
#ifndef SOAR_CLI_HAS_IMGUI
  // Built without the ImGui overlay (CI's no-imgui job): the window is a
  // bare video surface with no OSC, no overlays and no MRU, so there is
  // nothing to drive. The bare loop's own paths are pinned by the Escape
  // and WM_DELETE_WINDOW cases above.
  MESSAGE("app built without the ImGui overlay; skipping the HUD tour");
  return;
#else
  // Opt-in (the CI coverage job), same gating as the other X11 cases.
  if (std::getenv("SOAR_TEST_X11") == nullptr) {
    MESSAGE("SOAR_TEST_X11 not set; skipping the X11 window test");
    return;
  }
  if (std::system("command -v Xvfb >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1 && "
                  "python3 -c 'from Xlib.ext import xtest' >/dev/null 2>&1") != 0) {
    MESSAGE("Xvfb or python3-xlib missing; skipping the X11 window test");
    return;
  }

  // Same private-display scheme as the Escape case; cases run serially.
  const std::string suffix = std::to_string(70 + (::getpid() % 25));
  const std::string display = ":" + suffix;
  const std::string socket = "/tmp/.X11-unix/X" + suffix;

  pid_t xvfb = ::fork();
  REQUIRE(xvfb >= 0);
  if (xvfb == 0) {
    // Bigger than the other X11 cases: the drive tour opens combo popups
    // below the OSC row, which a 480px-tall screen would clip away.
    ::execlp("Xvfb", "Xvfb", display.c_str(), "-screen", "0", "1280x800x24",
             "-ac", "-nolisten", "tcp", static_cast<char*>(nullptr));
    _exit(127);
  }
  bool server_up = false;
  for (int i = 0; i < 50 && !server_up; ++i) {
    struct stat st;
    server_up = ::stat(socket.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
    if (!server_up) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  if (!server_up) {
    MESSAGE("Xvfb failed to start; skipping the X11 window test");
    ::kill(xvfb, SIGTERM);
    ::waitpid(xvfb, nullptr, 0);
    return;
  }

  pid_t injector = ::fork();
  REQUIRE(injector >= 0);
  if (injector == 0) {
    ::execlp("python3", "python3", "-c", kX11InjectorScript, display.c_str(),
             "soar", "drive", static_cast<char*>(nullptr));
    _exit(127);
  }

  // Keep the recent list out of the real user state dir: the run must
  // leave its MRU entries under the redirected XDG_STATE_HOME instead.
  const std::string state_home = "/tmp/soar_recent_xdg_" + std::to_string(::getpid());
  ::mkdir(state_home.c_str(), 0755);  // EEXIST from a prior run is fine
  // Seed the MRU with the two sources the tour reopens from the Recent
  // overlay: NullBackend's "noseek" URI stands in for a live stream
  // (opened, but every seek shortcut must degrade to a toast) and its
  // "fail-open" URI refuses to open at all (the deleted-file path). The
  // app records the CLI source at the front on startup, so the seeded
  // rows land at index 1 and 2 of the overlay.
  const std::string seed_dir = state_home + "/soar";
  ::mkdir(seed_dir.c_str(), 0755);
  {
    std::ofstream seed(seed_dir + "/recent.txt", std::ios::trunc);
    seed << "asset://sample\nasset://noseek-live\nasset://fail-open-x\n";
  }
  const ScopedEnv xdg_env("XDG_STATE_HOME", state_home.c_str());
  // The embedded bitmap font pins widget metrics so the injector's
  // coordinates mean the same thing on every machine.
  const ScopedEnv bitmap_font_env("SOAR_UI_BITMAP_FONT", "1");
  const ScopedEnv display_env("DISPLAY", display.c_str());
  const ScopedEnv audio_env("SDL_AUDIODRIVER", "dummy");
  // Null backend: the simulated 10-minute media gives the key tour a
  // stable target (pause → Paused, Right → seek) without decode timing.
  const auto run = runCli({"--backend=null", "asset://sample"});

  std::printf("x11 drive test: cli exit=%d, output:\n%s\n", run.exit_code, run.output.c_str());

  ::waitpid(injector, nullptr, 0);
  ::kill(xvfb, SIGTERM);
  ::waitpid(xvfb, nullptr, 0);

  if (run.output.find("SDL_CreateRenderer failed") != std::string::npos) {
    MESSAGE("no accelerated renderer under this X server; skipping");
    return;
  }
  REQUIRE(run.exit_code == 0);
  // The null backend opened and played (state=2), space paused it (1).
  CHECK(run.output.find("event: state=2") != std::string::npos);
  CHECK(run.output.find("event: state=1") != std::string::npos);
  // Right nudged the position while paused — any position event proves
  // the seek reached the backend and its event came back to the loop.
  CHECK(run.output.find("event: position=") != std::string::npos);
  // The Recent overlay reopened the seeded unseekable source: its media
  // info reports seekable=false, which is what turns the seek shortcuts
  // into a "Not seekable" toast instead of a position change.
  CHECK(run.output.find("seekable=false") != std::string::npos);
  // The third seeded entry refuses to open; the failure comes back as an
  // error event, which the UI turns into a toast plus the info overlay's
  // error row.
  CHECK(run.output.find(
            "event: error=open: simulated open failure for 'asset://fail-open-x'") !=
        std::string::npos);
  // The MRU list was rewritten under the redirected state home with all
  // three sources, most recently opened first.
  std::ifstream recent(seed_dir + "/recent.txt");
  if (!recent.good()) {
    FAIL("recent.txt was not written under XDG_STATE_HOME");
  } else {
    const std::string recent_txt((std::istreambuf_iterator<char>(recent)),
                                 std::istreambuf_iterator<char>());
    CHECK(recent_txt.find("asset://sample") != std::string::npos);
    CHECK(recent_txt.find("asset://noseek-live") != std::string::npos);
    CHECK(recent_txt.find("asset://fail-open-x") != std::string::npos);
    // The tour ends by reopening the sample, so it is back on top.
    CHECK(recent_txt.rfind("asset://sample", 0) == 0);
  }
#endif
#endif
}

TEST_CASE("windowed run over a stalled network source shows the buffering state") {
#ifdef _WIN32
  MESSAGE("the X11 window test is POSIX-only; skipping");
  return;
#else
#ifndef SOAR_CLI_HAS_IMGUI
  // The case quits through the HUD's own key handler and asserts on the
  // chip drawn from the event, both of which need the ImGui overlay.
  MESSAGE("app built without the ImGui overlay; skipping the buffering window test");
  return;
#else
  if (std::getenv("SOAR_TEST_X11") == nullptr) {
    MESSAGE("SOAR_TEST_X11 not set; skipping the X11 window test");
    return;
  }
#ifndef SOAR_WITH_FFMPEG
  MESSAGE("no FFmpeg backend; skipping the buffering window test");
  return;
#else
  std::string media;
  if (!envMediaPath("SOAR_TEST_AUDIO_ONLY", media)) {
    MESSAGE("SOAR_TEST_AUDIO_ONLY not set; skipping the buffering window test");
    return;
  }
  if (std::system("command -v Xvfb >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1 && "
                  "python3 -c 'from Xlib.ext import xtest' >/dev/null 2>&1") != 0) {
    MESSAGE("Xvfb or python3-xlib missing; skipping the buffering window test");
    return;
  }

  // Serves the fixture in two bursts 40s apart: playback drains the first
  // one, the backend's stall watchdog then reports BufferingStarted, and
  // the window's chip reads the atomic main.cpp mirrors it into.
  auto server = test_servers::startThrottledServer(media, 15200);
  if (server.pid < 0) {
    MESSAGE("throttled HTTP server failed to start; skipping");
    return;
  }

  const std::string suffix = std::to_string(95 + (::getpid() % 5));
  const std::string display = ":" + suffix;
  const std::string socket = "/tmp/.X11-unix/X" + suffix;

  pid_t xvfb = ::fork();
  REQUIRE(xvfb >= 0);
  if (xvfb == 0) {
    ::execlp("Xvfb", "Xvfb", display.c_str(), "-screen", "0", "1280x800x24",
             "-ac", "-nolisten", "tcp", static_cast<char*>(nullptr));
    _exit(127);
  }
  bool server_up = false;
  for (int i = 0; i < 50 && !server_up; ++i) {
    struct stat st;
    server_up = ::stat(socket.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
    if (!server_up) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  if (!server_up) {
    MESSAGE("Xvfb failed to start; skipping the buffering window test");
    ::kill(xvfb, SIGTERM);
    ::waitpid(xvfb, nullptr, 0);
    server.stop();
    return;
  }

  pid_t injector = ::fork();
  REQUIRE(injector >= 0);
  if (injector == 0) {
    ::execlp("python3", "python3", "-c", kX11InjectorScript, display.c_str(),
             "soar", "stall", static_cast<char*>(nullptr));
    _exit(127);
  }

  const std::string state_home = "/tmp/soar_stall_xdg_" + std::to_string(::getpid());
  ::mkdir(state_home.c_str(), 0755);
  const ScopedEnv xdg_env("XDG_STATE_HOME", state_home.c_str());
  const ScopedEnv display_env("DISPLAY", display.c_str());
  const ScopedEnv audio_env("SDL_AUDIODRIVER", "dummy");

  const std::string url = server.base_url + "/" + media.substr(media.find_last_of('/') + 1);
  const auto run = runCli({"--backend=ffmpeg", url});

  std::printf("x11 stall test: cli exit=%d, output:\n%s\n", run.exit_code,
              run.output.c_str());

  ::waitpid(injector, nullptr, 0);
  ::kill(xvfb, SIGTERM);
  ::waitpid(xvfb, nullptr, 0);
  server.stop();

  if (run.output.find("SDL_CreateRenderer failed") != std::string::npos) {
    MESSAGE("no accelerated renderer under this X server; skipping");
    return;
  }
  REQUIRE(run.exit_code == 0);
  // The window came up and started playing the network source ...
  CHECK(run.output.find("event: state=2") != std::string::npos);
  // ... and the stall crossed the backend's report threshold while the
  // window was up, which is the event the buffering chip is drawn from.
  CHECK(run.output.find("event: buffering started") != std::string::npos);
  // The subtitle shortcut on this audio-only source changed nothing: the
  // backend reported no track, so the HUD must have toasted instead of
  // selecting (a select would have shown up as a media-info event with a
  // non-negative subtitle id).
  const std::size_t last_info = run.output.rfind("event: media-info");
  REQUIRE(last_info != std::string::npos);
  CHECK(run.output.find("sub=-1", last_info) != std::string::npos);
#endif
#endif
#endif
}

TEST_CASE("windowed run with --cache-dir over a Range server shows download progress") {
  // P3c's windowed contract: playing an http:// source with --cache-dir
  // emits DownloadProgress as playback-paced block fetches fill the cache
  // (the download chip renders from exactly this event stream — the chip
  // itself stays up most of the run because a 0.5 MB fixture fills at
  // real-time decode pace). Same harness as the stalled-source case, but
  // an honest Range server and the download injector mode.
  std::string media;
  if (!envMediaPath("SOAR_TEST_AUDIO_ONLY", media)) {
    MESSAGE("SOAR_TEST_AUDIO_ONLY not set; skipping the download window test");
    return;
  }
#ifdef _WIN32
  MESSAGE("the X11 window test is POSIX-only; skipping");
  return;
#else
  if (std::getenv("SOAR_TEST_X11") == nullptr) {
    MESSAGE("SOAR_TEST_X11 not set; skipping the download window test");
    return;
  }
#ifndef SOAR_WITH_FFMPEG
  MESSAGE("no FFmpeg backend; skipping the download window test");
  return;
#else
  if (std::system("command -v Xvfb >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1 && "
                  "python3 -c 'from Xlib.ext import xtest' >/dev/null 2>&1") != 0) {
    MESSAGE("Xvfb or python3-xlib missing; skipping the download window test");
    return;
  }

  // Serve a copy so the fixture path is never a live server root.
  std::string tmpl = "/tmp/soar_cli_dlwin_XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  const char* tmp = ::mkdtemp(buf.data());
  REQUIRE(tmp != nullptr);
  const std::string dir(tmp);
  const std::string cache_dir = dir + "/cache";
  const std::string name = media.substr(media.find_last_of('/') + 1);
  REQUIRE(std::system(("cp '" + media + "' '" + dir + "/" + name + "'").c_str()) == 0);

  // 26000+: clear of the 24000s the http_cache suite uses and of the
  // 18000 + pid%2000 lottery port the python http.server case picks.
  test_servers::RangeServer srv = test_servers::startRangeServer(dir, 26000);
  if (srv.pid < 0) {
    MESSAGE("local Range HTTP server unavailable; skipping");
    REQUIRE(std::system(("rm -rf '" + dir + "'").c_str()) == 0);
    return;
  }

  const std::string suffix = std::to_string(80 + (::getpid() % 10));
  const std::string display = ":" + suffix;
  const std::string socket = "/tmp/.X11-unix/X" + suffix;

  pid_t xvfb = ::fork();
  REQUIRE(xvfb >= 0);
  if (xvfb == 0) {
    ::execlp("Xvfb", "Xvfb", display.c_str(), "-screen", "0", "1280x800x24",
             "-ac", "-nolisten", "tcp", static_cast<char*>(nullptr));
    _exit(127);
  }
  bool server_up = false;
  for (int i = 0; i < 50 && !server_up; ++i) {
    struct stat st;
    server_up = ::stat(socket.c_str(), &st) == 0 && S_ISSOCK(st.st_mode);
    if (!server_up) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  if (!server_up) {
    MESSAGE("Xvfb failed to start; skipping the download window test");
    ::kill(xvfb, SIGTERM);
    ::waitpid(xvfb, nullptr, 0);
    srv.stop();
    REQUIRE(std::system(("rm -rf '" + dir + "'").c_str()) == 0);
    return;
  }

  pid_t injector = ::fork();
  REQUIRE(injector >= 0);
  if (injector == 0) {
    ::execlp("python3", "python3", "-c", kX11InjectorScript, display.c_str(),
             "soar", "download", static_cast<char*>(nullptr));
    _exit(127);
  }

  // Keep the run out of the developer's real recent-files store.
  const std::string state_home = "/tmp/soar_dlwin_xdg_" + std::to_string(::getpid());
  ::mkdir(state_home.c_str(), 0755);
  const ScopedEnv xdg_env("XDG_STATE_HOME", state_home.c_str());
  const ScopedEnv display_env("DISPLAY", display.c_str());
  const ScopedEnv audio_env("SDL_AUDIODRIVER", "dummy");

  const std::string url = srv.base_url + "/" + name;
  const auto run =
      runCli({"--backend=ffmpeg", "--cache-dir=" + cache_dir, url});

  std::printf("x11 download test: cli exit=%d, output:\n%s\n", run.exit_code,
              run.output.c_str());

  ::waitpid(injector, nullptr, 0);
  ::kill(xvfb, SIGTERM);
  ::waitpid(xvfb, nullptr, 0);
  srv.stop();

  if (run.output.find("SDL_CreateRenderer failed") != std::string::npos) {
    MESSAGE("no accelerated renderer under this X server; skipping");
    REQUIRE(std::system(("rm -rf '" + dir + "'").c_str()) == 0);
    return;
  }
  REQUIRE(run.exit_code == 0);
  // The window came up and played the network source ...
  CHECK(run.output.find("event: state=2") != std::string::npos);
  // ... and the download arc ran to its terminal 100% event while the
  // window was up — the chip's draw branch rides the same payload.
  CHECK(run.output.find("event: download ") != std::string::npos);
  CHECK(run.output.find("event: download 100% (") != std::string::npos);

  REQUIRE(std::system(("rm -rf '" + dir + "'").c_str()) == 0);
#endif
#endif
}

TEST_CASE("headless FFmpeg run over subtitle-free media skips track selection") {
  // The headless flow only calls selectTrack/disableSubtitles when the
  // media actually carries a subtitle track (main.cpp guards on the
  // find_if result). The null-backend headless cases exercise the guarded
  // side through the sample's subtitle track; this case drives the
  // unguarded side over a real, subtitle-free file through the FFmpeg
  // backend — and with it the headless seek/pause/stop sequence on that
  // backend, which no other CLI case reaches.
  std::string media;
  if (!envMediaPath("SOAR_TEST_AUDIO_ONLY", media)) {
    MESSAGE("SOAR_TEST_AUDIO_ONLY not set; skipping");
    return;
  }
  const auto run = runCli({"--headless", "--backend=ffmpeg", media});
  if (run.output.find("Falling back to null backend") != std::string::npos) {
    MESSAGE("FFmpeg support not compiled in; skipping");
    return;
  }
  CHECK(run.exit_code == 0);
  CHECK(run.output.find("=== Media Info ===") != std::string::npos);
}

TEST_CASE("headless FFmpeg run over local HTTP server plays network source") {
  // First step of the network-playback roadmap (docs/mvp.md §5 P0): prove
  // that a plain http:// URL streams end to end through the FFmpeg backend
  // with no dedicated plumbing — avformat_open_input takes URLs as-is.
  // python http.server sends no Range replies, so the source reports
  // seekable=false, which also drives the headless flow's non-seekable
  // branch (main.cpp's mediaInfo().seekable guard) that no local file
  // reaches. POSIX-only: the server is a forked python3 sibling.
  std::string media;
  if (!envMediaPath("SOAR_TEST_AUDIO_ONLY", media)) {
    MESSAGE("SOAR_TEST_AUDIO_ONLY not set; skipping");
    return;
  }
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  (void)media;
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping");
    return;
  }
  const auto slash = media.find_last_of('/');
  const std::string dir = (slash == std::string::npos) ? std::string(".") : media.substr(0, slash);
  const std::string name = (slash == std::string::npos) ? media : media.substr(slash + 1);
  const std::string port = std::to_string(18000 + (::getpid() % 2000));

  const pid_t server = ::fork();
  REQUIRE(server >= 0);
  if (server == 0) {
    ::execlp("python3", "python3", "-m", "http.server", port.c_str(),
             "--bind", "127.0.0.1", "--directory", dir.c_str(),
             static_cast<char*>(nullptr));
    _exit(127);
  }

  // Wait until the server accepts connections before pointing the CLI at it.
  bool ready = false;
  for (int attempt = 0; attempt < 40 && !ready; ++attempt) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(static_cast<uint16_t>(std::stoi(port)));
      addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      ready = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
      ::close(fd);
    }
    if (!ready) {
      ::usleep(100 * 1000);
    }
  }

  if (!ready) {
    MESSAGE("local HTTP server failed to start; skipping");
    ::kill(server, SIGTERM);
    ::waitpid(server, nullptr, 0);
    return;
  }

  const auto run = runCli({"--headless", "--backend=ffmpeg",
                           "http://127.0.0.1:" + port + "/" + name});
  ::kill(server, SIGTERM);
  ::waitpid(server, nullptr, 0);

  if (run.output.find("Falling back to null backend") != std::string::npos) {
    MESSAGE("FFmpeg support not compiled in; skipping");
    return;
  }
  CHECK(run.exit_code == 0);
  CHECK(run.output.find("=== Media Info ===") != std::string::npos);
#endif
}

TEST_CASE("headless FFmpeg run over local HTTP server plays an HLS media playlist") {
  // Adaptive-protocol roadmap (docs/mvp.md §5 P2): an HLS media playlist
  // streams through the same URL pass-through as a plain file — the hls
  // demuxer fetches the relative segments from the same server with no
  // dedicated plumbing in the backend. The fixture is AAC: PCM cannot be
  // muxed into MPEG-TS, so segments would carry no playable stream.
  std::string media;
  if (!envMediaPath("SOAR_TEST_HLS_MEDIA", media)) {
    MESSAGE("SOAR_TEST_HLS_MEDIA not set; skipping");
    return;
  }
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  (void)media;
  return;
#else
  const auto slash = media.find_last_of('/');
  const std::string dir = (slash == std::string::npos) ? std::string(".") : media.substr(0, slash);
  const std::string name = (slash == std::string::npos) ? media : media.substr(slash + 1);
  auto run = runHeadlessOverHttpDir(dir, "/" + name, 18200);
  if (run.skipped) {
    MESSAGE("local HTTP server unavailable; skipping");
    return;
  }
  if (run.cli.output.find("Falling back to null backend") != std::string::npos) {
    MESSAGE("FFmpeg support not compiled in; skipping");
    return;
  }
  CHECK(run.cli.exit_code == 0);
  CHECK(run.cli.output.find("=== Media Info ===") != std::string::npos);
  CHECK(run.cli.output.find("Duration: 6") != std::string::npos);
#endif
}

TEST_CASE("headless FFmpeg run over local HTTP server plays a multi-variant HLS master playlist") {
  // The master playlist references two variants (128k stereo, 48k mono).
  // Which variants FFmpeg exposes as streams and which one it plays is
  // demuxer-internal and version-dependent (FFmpeg surfaces both today),
  // so the assertion pins playability and a track listing, not the
  // variant count (docs/coverage-notes.md §3.3).
  std::string media;
  if (!envMediaPath("SOAR_TEST_HLS_MASTER", media)) {
    MESSAGE("SOAR_TEST_HLS_MASTER not set; skipping");
    return;
  }
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  (void)media;
  return;
#else
  const auto slash = media.find_last_of('/');
  const std::string dir = (slash == std::string::npos) ? std::string(".") : media.substr(0, slash);
  const std::string name = (slash == std::string::npos) ? media : media.substr(slash + 1);
  auto run = runHeadlessOverHttpDir(dir, "/" + name, 18300);
  if (run.skipped) {
    MESSAGE("local HTTP server unavailable; skipping");
    return;
  }
  if (run.cli.output.find("Falling back to null backend") != std::string::npos) {
    MESSAGE("FFmpeg support not compiled in; skipping");
    return;
  }
  CHECK(run.cli.exit_code == 0);
  CHECK(run.cli.output.find("=== Media Info ===") != std::string::npos);
  CHECK(run.cli.output.find("Tracks: ") != std::string::npos);
#endif
}

TEST_CASE("headless FFmpeg run over local HTTP server plays a DASH manifest") {
  // DASH joins HLS on the same pass-through: the dash demuxer parses the
  // MPD and fetches init/segment m4s from the same server. Needs
  // FFmpeg's dash demuxer (libxml2) — the apt builds on CI and the local
  // vcpkg-adjacent build both carry it.
  std::string media;
  if (!envMediaPath("SOAR_TEST_DASH_AUDIO", media)) {
    MESSAGE("SOAR_TEST_DASH_AUDIO not set; skipping");
    return;
  }
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  (void)media;
  return;
#else
  const auto slash = media.find_last_of('/');
  const std::string dir = (slash == std::string::npos) ? std::string(".") : media.substr(0, slash);
  const std::string name = (slash == std::string::npos) ? media : media.substr(slash + 1);
  auto run = runHeadlessOverHttpDir(dir, "/" + name, 18400);
  if (run.skipped) {
    MESSAGE("local HTTP server unavailable; skipping");
    return;
  }
  if (run.cli.output.find("Falling back to null backend") != std::string::npos) {
    MESSAGE("FFmpeg support not compiled in; skipping");
    return;
  }
  CHECK(run.cli.exit_code == 0);
  CHECK(run.cli.output.find("=== Media Info ===") != std::string::npos);
  CHECK(run.cli.output.find("Duration: 6") != std::string::npos);
#endif
}

TEST_CASE("headless FFmpeg run over a local RTSP server plays a live audio stream") {
  // RTSP is the third pass-through protocol: avformat_open_input handles the
  // whole rtsp:// handshake (OPTIONS/DESCRIBE/SETUP/PLAY) and then reads the
  // RTP stream. Duration is N/A for a live source and the headless seek is
  // expected to be a no-op, so the assertions pin the contract (open, one
  // audio track, PCMA codec) and not transport-level metadata.
#ifndef _WIN32
  auto run = runHeadlessOverRtsp(18500);
  if (run.skipped) {
    MESSAGE("local RTSP server unavailable; skipping");
    return;
  }
  if (run.cli.output.find("Falling back to null backend") != std::string::npos) {
    MESSAGE("FFmpeg support not compiled in; skipping");
    return;
  }
  CHECK(run.cli.exit_code == 0);
  CHECK(run.cli.output.find("Using FFmpeg backend") != std::string::npos);
  CHECK(run.cli.output.find("=== Media Info ===") != std::string::npos);
  CHECK(run.cli.output.find("Tracks: 1") != std::string::npos);
  CHECK(run.cli.output.find("pcm_alaw") != std::string::npos);
#else
  MESSAGE("POSIX-only test; skipping");
#endif
}

TEST_CASE("ffmpeg request degrades gracefully when unavailable") {
  const auto run = runCli({"--headless", "--backend=ffmpeg", "asset://sample"});
#ifdef SOAR_WITH_FFMPEG
  // With the real backend the magic asset:// URI is not openable media.
  CHECK(run.exit_code == 1);
  CHECK(run.output.find("Failed to open source") != std::string::npos);
#else
  // Without FFmpeg support the CLI falls back to the null backend.
  CHECK(run.exit_code == 0);
  CHECK(run.output.find("Falling back to null backend") != std::string::npos);
#endif
}

TEST_CASE("headless FFmpeg run with --cache-dir leaves a meta/data cache pair") {
  // P3a's CLI contract: --cache-dir=<dir> with an http:// source routes the
  // download through the disk cache, and exactly one .meta/.data pair
  // exists afterwards. The server must speak Range — python http.server
  // does not, and the cache rejects range-less servers outright — so this
  // reuses the Range-capable fixture from test_http_servers.h.
  std::string media;
  if (!envMediaPath("SOAR_TEST_AUDIO_ONLY", media)) {
    MESSAGE("SOAR_TEST_AUDIO_ONLY not set; skipping");
    return;
  }
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  (void)media;
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping");
    return;
  }
  const auto slash = media.find_last_of('/');
  const std::string name = (slash == std::string::npos) ? media : media.substr(slash + 1);

  // Serve a copy so the fixture path is never a live server root, and keep
  // the cache in its own subdirectory of the scratch dir.
  std::string tmpl = "/tmp/soar_cli_cache_XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  const char* tmp = ::mkdtemp(buf.data());
  REQUIRE(tmp != nullptr);
  const std::string dir(tmp);
  const std::string cache_dir = dir + "/cache";
  REQUIRE(std::system(("cp '" + media + "' '" + dir + "/" + name + "'").c_str()) == 0);

  const std::string port = std::to_string(19000 + (::getpid() % 200));
  // RAII server (same fixture as the disk-cache suite): its destructor
  // reaps the child even when a REQUIRE below jumps out of the case.
  test_servers::RangeServer srv =
      test_servers::startRangeServer(dir, 19000);
  if (srv.pid < 0) {
    MESSAGE("local Range HTTP server unavailable; skipping");
    REQUIRE(std::system(("rm -rf '" + dir + "'").c_str()) == 0);
    return;
  }

  const auto run = runCli({"--headless", "--backend=ffmpeg",
                           "--cache-dir=" + cache_dir,
                           "http://127.0.0.1:" + port + "/" + name});

  srv.stop();

  if (run.output.find("Falling back to null backend") != std::string::npos) {
    MESSAGE("FFmpeg support not compiled in; skipping");
    REQUIRE(std::system(("rm -rf '" + dir + "'").c_str()) == 0);
    return;
  }
  CHECK(run.exit_code == 0);
  CHECK(run.output.find("=== Media Info ===") != std::string::npos);

  // Exactly one cache entry: the meta names the source, the data holds it.
  int metas = 0;
  int datas = 0;
  if (DIR* d = ::opendir(cache_dir.c_str())) {
    while (const dirent* e = ::readdir(d)) {
      const std::string n = e->d_name;
      if (n.size() > 5 && n.compare(n.size() - 5, 5, ".meta") == 0) ++metas;
      if (n.size() > 5 && n.compare(n.size() - 5, 5, ".data") == 0) ++datas;
    }
    ::closedir(d);
  }
  CHECK(metas == 1);
  CHECK(datas == 1);

  REQUIRE(std::system(("rm -rf '" + dir + "'").c_str()) == 0);
#endif
}
