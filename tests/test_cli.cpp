// Integration tests for the soar CLI (src/app/main.cpp). Each case runs
// the real executable as a subprocess and asserts the exit code and the
// combined stdout/stderr report. The SDL window block is reached three
// deterministic ways: the dummy video driver drives the renderer-failure
// path on every platform, and — where SOAR_TEST_X11 opts in (the CI
// coverage job) — a real Xvfb server plays the render loop to a clean
// exit, either through an XTEST Escape injection or through a
// WM_DELETE_WINDOW client message, which SDL turns into SDL_QUIT even
// with no window manager present, so the loop's coverage lands in the
// same run instead of being written off as untestable.
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
// In both modes the injector exits 0 once the window disappears (the CLI
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
#ifndef _WIN32
  // A sanitizer-instrumented child can hang indefinitely: observed once in
  // CI, where the tsan CLI stalled right after fetching an HLS master's
  // segments and burned the entire 300s CTest budget. Cap every run so a
  // hung child degrades into a normal failing assertion (exit 124) with
  // whatever output it produced, instead of a context-free suite timeout.
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
             "soar (skeleton)", "escape", static_cast<char*>(nullptr));
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
             "soar (skeleton)", "delete", static_cast<char*>(nullptr));
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
