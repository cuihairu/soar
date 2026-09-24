// Integration tests for the soar CLI (src/app/main.cpp). Each case runs
// the real executable as a subprocess and asserts the exit code and the
// combined stdout/stderr report. The SDL window block is reached two
// deterministic ways: the dummy video driver drives the renderer-failure
// path on every platform, and — where SOAR_TEST_X11 opts in (the CI
// coverage job) — a real Xvfb server plus an XTEST Escape injection plays
// the render loop to a clean exit, so the loop's coverage lands in the
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
#  include <signal.h>
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

// One-shot injector for the X11 window test. It waits for the soar window
// to map, focuses it explicitly — no window manager runs under Xvfb, so
// nothing owns input focus by default — and delivers a genuine Escape
// through the XTEST extension until the window disappears (the CLI exits
// cleanly on Escape). Decoding throttles to real time, so the script
// waits out the resolution changes of the multi-res fixture before
// pressing anything, letting the render loop exercise its texture
// re-creation branch. Coverage builds decode slower than the wall clock
// (-O0 instrumentation), so the wait must be measured in decode time,
// not decode speed assumptions: 4.5s crosses both boundaries even when
// the decoder runs at half the presentation pace.
const char* const kX11EscapeScript = R"PY(import sys, time

from Xlib import X, display
from Xlib.ext import xtest

disp_name, title = sys.argv[1], sys.argv[2]
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
    ::execlp("python3", "python3", "-c", kX11EscapeScript, display.c_str(),
             "soar (skeleton)", static_cast<char*>(nullptr));
    _exit(127);
  }

  const ScopedEnv display_env("DISPLAY", display.c_str());
  // Same reason as the sanitizer jobs: keep libpulse out of the picture.
  const ScopedEnv audio_env("SDL_AUDIODRIVER", "dummy");
  const auto run = runCli({"--backend=ffmpeg", media});

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
