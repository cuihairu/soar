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

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
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
// "rebind" — key-rebinding end to end: press the original key (argv[4],
//   hex keysym) four times — the window must survive the storm (its
//   binding was taken over; exit 7 proves it did not) — then press the
//   rebound key (argv[5]) and wait for the window to disappear (exit 0).
//   Same contract as escape/delete: non-zero means the injector could
//   not do its job.
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

# Window-focused injection primitives, shared by the drive modes. Every
# primitive re-finds the window first: the XID can go stale if SDL
# recreates the window (fullscreen round-trips), and a tour step aimed at
# a dead window would silently do nothing (the helpers rebind the global
# win; a vanished window aborts the tour with the deadline exit code).
def focus():
    global win
    win = find_window()
    if win is None:
        return False
    try:
        win.set_input_focus(X.RevertToParent, X.CurrentTime)
        d.sync()
        return True
    except Exception:
        return False


def key(kc):
    global win
    win = find_window()
    if win is None:
        return False
    try:
        xtest.fake_input(d, X.KeyPress, kc)
        d.sync()
        xtest.fake_input(d, X.KeyRelease, kc)
        d.sync()
        return True
    except Exception:
        return False


def button(btn):
    global win
    win = find_window()
    if win is None:
        return False
    try:
        xtest.fake_input(d, X.ButtonPress, btn)
        d.sync()
        xtest.fake_input(d, X.ButtonRelease, btn)
        d.sync()
        return True
    except Exception:
        return False


def holdclick(btn, hold=0.35):
    # A press/release pair only becomes an ImGui click when a frame
    # boundary falls strictly between the two events: both halves pumped
    # inside one frame net out to "mouse up", and ImGui never sees the
    # press at all. (The app's own SDL handlers see every event either
    # way, which is why the OSC toggle fires while a combo pick does
    # not.) A back-to-back XTEST pair is a few milliseconds apart, so
    # under the -O0 coverage build's long frames most clicks vanish that
    # way — measured there, 2 of 11 combo opens landed with button(),
    # every one landed with a hold well over a frame time.
    global win
    win = find_window()
    if win is None:
        return False
    try:
        xtest.fake_input(d, X.ButtonPress, btn)
        d.sync()
        time.sleep(hold)
        xtest.fake_input(d, X.ButtonRelease, btn)
        d.sync()
        return True
    except Exception:
        return False


def moved(x, y):
    # python-xlib: warp_pointer(x, y, ...) — dest is relative to the
    # window; no source-rectangle filtering needed.
    global win
    win = find_window()
    if win is None:
        return False
    try:
        win.warp_pointer(x, y)
        d.sync()
        return True
    except Exception:
        return False


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

if mode == "empty":
    # Source-less window (BUGS.md #3): the GUI entry starts with no media
    # and the poster is the open entry. Exercise both entries — the O key
    # (dialog on Windows, toast here) and a click over the video area —
    # then Escape out. No decode runs, so a short settle wait is enough.
    time.sleep(2.5)
    geo = None
    try:
        geo = win.get_geometry()
    except Exception:
        pass
    if not focus():
        sys.exit(4)
    if not key(d.keysym_to_keycode(0x6F)):  # 'o'
        sys.exit(4)
    time.sleep(0.4)
    if geo is not None:
        if not moved(geo.width // 2, geo.height // 2):
            sys.exit(4)
        time.sleep(0.3)
        if not button(1):
            sys.exit(4)
        time.sleep(0.4)
    if not focus():
        sys.exit(4)
    if not key(d.keysym_to_keycode(0xFF1B)):  # Escape
        sys.exit(4)
    deadline = time.monotonic() + 15.0
    while time.monotonic() < deadline:
        if find_window() is None:
            sys.exit(0)
        time.sleep(0.2)
    sys.exit(5)

if mode == "rebind":
    # Key-rebinding end to end: the original key (argv[4], hex keysym)
    # must not close the window, the rebound key (argv[5]) must. Same
    # choreography as the escape storm below — the 4.5s settle, one click
    # on the bare video area (a popup left open would own the keyboard
    # and the app's popup guard swallows every shortcut), then key
    # storms rather than single presses.
    old_kc = d.keysym_to_keycode(int(sys.argv[4], 0))
    new_kc = d.keysym_to_keycode(int(sys.argv[5], 0))
    time.sleep(4.5)
    if moved(200, 400):
        button(1)
        time.sleep(0.5)
    for _ in range(4):  # the old key must do nothing
        if not focus() or not key(old_kc):
            sys.exit(4)
        time.sleep(0.5)
    if find_window() is None:
        sys.exit(7)  # the old key still quits: the rebinding did not take
    deadline = time.monotonic() + 20.0
    while time.monotonic() < deadline:
        if not focus() or not key(new_kc):
            sys.exit(4)
        time.sleep(0.5)
        if find_window() is None:
            sys.exit(0)  # the rebound key quit: rebinding verified end to end
    sys.exit(5)

if mode == "drive":
    # Let the UI come up and the null backend start before touching it.
    time.sleep(3.0)

    try:
        geo = win.get_geometry()
        cx, cy = geo.width // 2, geo.height // 2
        # Key tour: overlays open+close, fullscreen round-trip, volume and
        # mute, subtitle offset nudge ([ ]) and visibility toggle (v),
        # a screenshot attempt on the frame-less null backend (s),
        # track cycling (subtitles on then off), the seek family
        # (Home/Left/Right/PageUp/PageDown/50%), pause/resume, an
        # end-of-media round trip (90% + PageDown lands exactly on the
        # 10-minute mark -> Ended; space replays), rate down/up.
        tour = (0x69, 0x69, 0x72, 0x72, 0x68, 0x68,
                0x66, 0xFF1B,
                0xFF52, 0xFF54, 0x6D, 0x6D,
                0x5B, 0x5D, 0x73, 0x73,
                0x61, 0x63, 0x63,
                0xFF50, 0xFF51, 0xFF53, 0xFF55, 0xFF56, 0x35,
                0x20, 0x20,
                0x39, 0xFF56,
                0x20,
                0x2C, 0x2C, 0x2E, 0x2E)
        for keysym in tour:
            if not focus():
                sys.exit(4)
            if not key(d.keysym_to_keycode(keysym)):
                sys.exit(4)
            time.sleep(0.35)
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
        wclick(248, 495)   # Mute (row gained |< >| after Stop; the clock
                           # used to sit between them but moved to the
                           # seek row, so everything here shifted left by
                           # its width; label widens to "Unmute" and the
                           # volume slider shifts right — the drag below
                           # stays in the intersection of both variants)
        moved(299, 495); time.sleep(0.15)
        xtest.fake_input(d, X.ButtonPress, 1); d.sync(); time.sleep(0.1)
        moved(324, 495); time.sleep(0.1)
        xtest.fake_input(d, X.ButtonRelease, 1); d.sync(); time.sleep(0.3)
        wclick(378, 495)   # rate combo open (popup flips up: no room below)
        focus(); key(d.keysym_to_keycode(0x69)); time.sleep(0.2)  # suppressed
        wclick(378, 468)   # pick 2x (bottom row, clear of the 1.0x default)
        wclick(449, 495)   # audio combo open (single item fits below)
        wclick(478, 522)   # pick the audio track
        wclick(527, 495)   # subtitle combo open (flips up)
        wclick(549, 468)   # pick the subtitle track ...
        wclick(527, 495)   # ... reopen; "Off" only acts as a change when a
        wclick(549, 443)   # track is currently selected, so the order matters
        wclick(586, 495)   # Sub overlay (the Sub button sits between the
        wclick(586, 495)   # subtitle combo and Info; open, then close)
        wclick(630, 495)   # Info overlay (shifted right by the Sub button)
        wclick(630, 495)   # (close again)
        wclick(682, 495)   # fullscreen toggle button (also shifted)
        focus(); key(d.keysym_to_keycode(0xFF1B)); time.sleep(0.3)
        # Seek bar: the row-1 rail sits at y 452..477 under the OSC top
        # edge, flanked by the elapsed/remaining timecodes (they narrowed
        # it; the drag points stay well inside). A press-drag-back-release
        # first (the value returns to where it started, so the gesture is a
        # cancelled drag that must not seek), then a press-drag-release so
        # the scrub preview and the release-commit arcs both run.
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

if mode == "osc":
    # The OSC walkthrough over the null backend. The auto-hide verdict is
    # read from the teed trail's logical-visibility edges (event: osc=...)
    # instead of pixels: hidden after the 2.5s idle while playing, shown
    # again on mouse motion alone (motion is input since the OSC batch),
    # hidden again once the idle window restarted by that motion lapses,
    # shown on the next key (which pauses and pins the bar for the click
    # phase). The row-2 buttons then fire through the toast mirror:
    # prev/next walk both queue boundaries AND a real handover (the
    # launcher queued noseek-live behind sample), the A-B arm, the CC
    # caption-visibility toggle, and the null backend's screenshot
    # refusal. Every wait failure records a code and falls through to
    # the quit storm — leaving the CLI playing would only burn the
    # test's own 300s budget.
    tee = sys.argv[4] if len(sys.argv) > 4 else ""

    def marker_count(marker):
        try:
            with open(tee) as f:
                return f.read().count(marker)
        except OSError:
            return 0

    def wait_ge(marker, want, budget):
        deadline = time.monotonic() + budget
        while time.monotonic() < deadline:
            if marker_count(marker) >= want:
                return True
            time.sleep(0.2)
        return False

    rc = 0
    time.sleep(3.0)  # UI up, null media playing, bar freshly idle
    geo = win.get_geometry()
    cx, cy = geo.width // 2, geo.height // 2
    try:
        if not wait_ge("event: osc=hidden", 1, 20.0):
            rc = 6
        # Mouse motion alone must bring the bar back... Xvfb parks the
        # pointer at the screen center and the window is centered too, so
        # a warp aimed straight at the center is a zero-length move on
        # the first run — XWarpPointer emits no event for one, and no
        # event means no noteActivity and no wake. Step through a second
        # point that differs from both the park position and the target:
        # at least one of the two warps is a real move, so the wake edge
        # arrives regardless of where the pointer started.
        moved(cx - 400, cy - 180)
        moved(cx, cy)
        if not wait_ge("event: osc=shown", 1, 5.0):
            rc = 6
        # ...and the hide delay restarted from that motion: 4s of no
        # input (2.5s delay + fade margin) hides it again.
        if not wait_ge("event: osc=hidden", 2, 8.0):
            rc = 6
        # A key is activity too; space pauses, and the pause pins the
        # bar for the button phase below.
        focus(); key(d.keysym_to_keycode(0x20))
        if not wait_ge("event: osc=shown", 2, 5.0):
            rc = 6
        # Row 2 buttons (bitmap-font coordinates, same derivation as the
        # drive tour's widget phase). holdclick, not bare pairs: the -O0
        # frames swallow back-to-back press/release. The queue holds two
        # null sources (the launcher queued noseek-live behind sample), so
        # prev/next walk a real boundary AND a real handover: the handover
        # is read from the trail — noseek-live's media-info is the only
        # seekable=false line, and each open adds a media-info line.
        def bclick(x, y):
            # Settle after the warp. The app's click handler decides
            # "was this a widget click" from ImGui's WantCaptureMouse,
            # which is decided at the previous frame's end from the mouse
            # position THEN in effect: a press that lands in the same
            # frame as the warp still reads the pre-warp position, misses
            # the bar, and toggles the OSC instead of reaching the button
            # (the click then lands on the bar anyway, so the action and
            # the toggle both fire). The drive tour's wclick sleeps for
            # the same reason; holdclick keeps the press/release apart by
            # more than a frame.
            moved(x, y)
            time.sleep(0.25)
            holdclick(1)

        bclick(163, 495)   # |< prev: queue head boundary
        if not wait_ge("event: toast=Start of playlist", 1, 5.0):
            rc = 7
        bclick(201, 495)   # >| next: opens the second entry
        if not wait_ge("seekable=false", 1, 8.0):
            rc = 7
        bclick(201, 495)   # >| next again: tail boundary
        if not wait_ge("event: toast=End of playlist", 1, 5.0):
            rc = 7
        bclick(163, 495)   # |< prev: back to the sample
        if not wait_ge("event: media-info", 3, 8.0):
            rc = 7
        bclick(732, 495)   # A-B: arm point A
        if not wait_ge("event: toast=Loop point A set at", 1, 5.0):
            rc = 7
        bclick(732, 495)   # A-B again: pair settles somehow
        if not wait_ge("event: toast=", 4, 5.0):  # Start, End, arm, 2nd
            rc = 7
        bclick(769, 495)   # CC: caption visibility off (the V shortcut's action)
        if not wait_ge("event: toast=Subtitles off", 1, 5.0):
            rc = 7
        bclick(769, 495)   # CC again: back on
        if not wait_ge("event: toast=Subtitles on", 1, 5.0):
            rc = 7
        # The paused overlay's round play button (drawn at the window
        # center, 16px above it): a click resumes, exactly like Space.
        # The bar stays up for the Shot click below — the resume restarts
        # the idle window, which is longer than the click gap.
        bclick(480, 254)
        if not wait_ge("event: state=2", 2, 5.0):
            rc = 7
        # The clock moved from row 2 up to the seek row, so everything
        # right of it shifted left by its width (~106px). Re-probed
        # against the live bar: A-B's face spans window x 710..745, CC
        # 755..783, Shot 793..834 (the bar edge stays 940).
        bclick(811, 495)   # Shot: no frames on the null backend
        if not wait_ge("event: toast=Screenshot needs the FFmpeg backend", 1, 5.0):
            rc = 7
    except Exception:
        pass  # best effort: whatever ran before an error still counts
    qk = d.keysym_to_keycode(0x71)
    deadline = time.monotonic() + 60.0
    while time.monotonic() < deadline:
        try:
            focus()
            key(qk)
        except Exception:
            sys.exit(rc if rc else 0)
        time.sleep(0.5)
        if find_window() is None:
            sys.exit(rc if rc else 0)
    sys.exit(rc if rc else 4)

if mode == "subsdrive":
    # Real-backend window run over the SRT fixture. The HUD tour rides the
    # null backend, so drawSubtitles and the Subtitle Settings page never
    # see a decoded frame or a real track list there; this mode pauses
    # inside the first cue's display window and then drives the subtitle
    # path end to end — offset nudges, the visibility toggle, the settings
    # page (font size, sync offset, visibility checkbox) and the track
    # combo (Off disables the stream, the track row re-selects it).
    # Coordinates pinned by SOAR_UI_BITMAP_FONT=1 (probe screenshots).
    time.sleep(2.2)
    focus(); key(d.keysym_to_keycode(0x20))  # space: freeze inside a cue
    time.sleep(0.6)
    # [ ] nudge the sync offset, v toggles visibility twice (both toast
    # arms), s takes the screenshot (mpv binding) — all keyboard paths
    # behind the Subtitle Settings page.
    for keysym in (0x5B, 0x5D, 0x76, 0x76, 0x73):
        if not focus():
            sys.exit(4)
        key(d.keysym_to_keycode(keysym))
        time.sleep(0.4)

    # Hold-clicks everywhere (every mode's sclick below uses the same
    # shape): this tour runs against the -O0 coverage build, whose frame
    # time can exceed the few milliseconds between a back-to-back press
    # and release — the pair is then pumped inside a single frame, nets
    # out to "mouse up", and ImGui never registers the click at all (see
    # holdclick). When the lost click is a combo pick, the popup stays
    # open and owns the keyboard (the popup-guard mechanism the quit
    # loop's dismissal works around); a hold crosses at least one frame
    # boundary, so every click below lands.
    def sclick(x, y):
        moved(x, y); time.sleep(0.25)
        holdclick(1); time.sleep(0.9)

    sclick(580, 495)   # Sub button: open the Subtitle Settings page
    sclick(360, 232)   # font size slider: jump to the small end
    sclick(630, 232)   # ... and to the large end
    for fx in (415, 479, 512, 544):  # ... and the 20/28/32/36 px buckets,
        sclick(fx, 232)              # each one arm of drawSubtitles' font chain
    sclick(595, 263)   # sync offset slider: a large positive offset puts
                       # the paused position past the cue (expiry arc) ...
    sclick(360, 263)   # ... negative puts it before (not-yet arc) ...
    sclick(495, 263)   # ... and the middle puts it back inside
    sclick(335, 295)   # visibility checkbox off (renderer early-out) ...
    sclick(335, 295)   # ... and back on
    sclick(459, 183)   # track combo open
    sclick(459, 248)   # pick the track row (subtitle selectTrack)
    sclick(459, 183)   # reopen; "Off" has a selection to act on now ...
    sclick(459, 221)   # ... (disableSubtitles on the real backend)
    sclick(459, 183)   # reopen; nothing is selected, so this "Off" ...
    sclick(459, 221)   # ... pick is the no-op arm
    sclick(459, 183)   # reopen ...
    sclick(459, 248)   # ... and re-select the track row
    sclick(580, 495)   # close the page
    time.sleep(0.5)
    # The OSC copy of the subtitle picker over the same real track list.
    # The first pick is a no-op by construction (the page above left the
    # stream selected, and a row that already is the selection
    # short-circuits); the pair after it is the pair that matters, since
    # "Off" only acts while something is selected.
    # Row centers re-probed after the OSC row-2 rework against the live
    # popup, then shifted 106px left when the clock moved to the seek
    # row: the combo sits at x 484-554 (click 517) and the popup flips
    # up anchored to it, so the Off row's text band spans y 414-422
    # (click 418) and the single stream row y 440-450 (click 445), both
    # clicked at x 514 inside the popup's left half (SOAR_UI_BITMAP_FONT=1).
    sclick(517, 495)   # subtitle combo open (Off + the stream, flips up)
    sclick(514, 445)   # the track row, already selected: no-op arm
    sclick(517, 495)   # reopen; "Off" has something to act on now ...
    sclick(514, 418)   # ... (disableSubtitles from the OSC)
    sclick(517, 495)   # reopen; nothing is selected, so "Off" ...
    sclick(514, 418)   # ... is the no-op arm again
    sclick(517, 495)   # reopen ...
    sclick(514, 445)   # ... and re-select the track row
    # Popup dismissal before the key phase (same defense as the quit loop
    # below): under -O0 coverage instrumentation a back-to-back click pair
    # can vanish inside one frame (see holdclick), a missed pick leaves
    # the combo open, and a combo popup owns the keyboard — every key from
    # here on (C, I, H, M, L, q) would be swallowed by the app's popup
    # guard. One hold-click on the bare video area closes whatever is
    # open; it only toggles the OSC, and the > 500 ms release spacing
    # keeps it from reading as the double-click fullscreen toggle.
    if moved(200, 400):
        holdclick(1)
    time.sleep(0.7)
    # Key phase over the same tracks: C cycles off -> on -> off around
    # the single stream, A wraps a one-stream audio list straight back
    # onto itself, and I opens Media Info -- the one place the whole
    # track list is spelled out with codec, language and the selected
    # marker, which the null-backend tour can never show.
    for keysym in (0x63, 0x63, 0x61):
        if not focus():
            sys.exit(4)
        key(d.keysym_to_keycode(keysym))
        time.sleep(0.5)
    focus(); key(d.keysym_to_keycode(0x69))
    time.sleep(0.8)
    focus(); key(d.keysym_to_keycode(0x69))
    time.sleep(0.3)
    # The Help overlay's shortcut table (BeginTable body) and Media Info's
    # muted rendering both need a state the phases above never held: Help
    # open at all, and Info open while audio is muted.
    focus(); key(d.keysym_to_keycode(0x68))  # h: Help overlay
    time.sleep(0.9)
    focus(); key(d.keysym_to_keycode(0x68))  # ... and close it
    time.sleep(0.3)
    focus(); key(d.keysym_to_keycode(0x6D))  # m: mute ...
    time.sleep(0.4)
    focus(); key(d.keysym_to_keycode(0x69))  # ... so Info shows "(muted)"
    time.sleep(0.8)
    focus(); key(d.keysym_to_keycode(0x69))  # ... and close it
    time.sleep(0.3)
    # Second dismissal, then resume playback: the space at the top of the
    # tour froze the clock, and cycleLoopAB's B press compares against
    # point A through the live position -- with a frozen clock every B
    # press is A == B and only the invalid-window toast arm runs. The
    # three presses below need the clock moving: A, then B a second later
    # (a real A < B window arms the loop), then the armed press clears it.
    if moved(200, 400):
        holdclick(1)
    time.sleep(0.7)
    focus(); key(d.keysym_to_keycode(0x20))  # space: resume
    time.sleep(1.2)
    # A-B loop UI (L key): cycle arm-A -> arm-B -> clear. This exercises
    # PlayerHud::cycleLoopAB, the HUD loop display, and the backend's
    # setLoopAB/clearLoopAB/loopAB path on the real FFmpeg backend.
    focus(); key(d.keysym_to_keycode(0x6C))  # L: set point A
    time.sleep(1.0)
    focus(); key(d.keysym_to_keycode(0x6C))  # L: set point B
    time.sleep(1.0)
    # While the loop is armed, Media Info spells out the A - B window —
    # the overlay's loop row only renders in that state.
    focus(); key(d.keysym_to_keycode(0x69))
    time.sleep(0.8)
    focus(); key(d.keysym_to_keycode(0x69))
    time.sleep(0.3)
    focus(); key(d.keysym_to_keycode(0x6C))  # L: clear the loop
    time.sleep(0.5)

    qk = d.keysym_to_keycode(0x71)
    deadline = time.monotonic() + 60.0
    while time.monotonic() < deadline:
        # A combo popup left open by a lost click owns the keyboard —
        # the app's popup guard swallows q/Escape while it is up (the wedge
        # that hung the coverage CI runs mid-tour). A single hold-click on
        # the bare video area dismisses any popup first; it only toggles
        # the OSC, and the > 500 ms release spacing keeps two loop clicks
        # from ever reading as the double-click fullscreen toggle.
        if moved(200, 400):
            holdclick(1)
        time.sleep(0.7)
        try:
            focus()
            key(qk)
        except Exception:
            sys.exit(0)
        time.sleep(0.5)
        if find_window() is None:
            sys.exit(0)
    sys.exit(4)

if mode == "subsdl":
    # The subtitle-download pick over a local catalog server (the
    # SOAR_SUBTITLE_* envs the test set): the media directory has no
    # sidecars, so under the separator the combo offers the fixture's
    # embedded srt row and then "[download]" — the same slot the sidecar
    # candidate row occupies in the sidecar mode, same coordinates. One
    # pick runs fetch -> store (system temp) -> loadExternalSubtitle, and
    # the selection trail then shows the external id. The repeat guard on
    # the pick is the sidecar case's: a lag frame swallowed the first
    # click leaves the popup up and this re-picks; on the happy path the
    # popup is closed and it lands on the page's slider spot, harmless.
    # Coordinates probed under SOAR_UI_BITMAP_FONT=1.
    time.sleep(2.2)
    focus(); key(d.keysym_to_keycode(0x20))  # space: wake the HUD, pause
    time.sleep(0.8)

    def sclick(x, y):
        moved(x, y); time.sleep(0.25)
        holdclick(1); time.sleep(0.9)

    sclick(580, 495)   # Sub button: open the Subtitle Settings page
    time.sleep(0.9)
    sclick(459, 190)   # Track combo: popup = Off / srt / sep / [download]
    time.sleep(1.0)
    sclick(460, 274)   # the [download] row -> fetch + store + load
    time.sleep(2.0)    # room for the loopback request round trip
    # Repeat guard: if a lag frame swallowed the pick above, the popup is
    # still up and this re-picks the row. On the normal path the popup is
    # closed and this lands on the page's Sync-offset slider (same spot,
    # different context) — a setting this case never asserts on, and the
    # per-pid XDG_STATE_HOME keeps it from leaking anywhere.
    sclick(460, 274)
    time.sleep(1.0)
    sclick(200, 400)   # bare video area: dismiss any popup, toggle nothing
    time.sleep(0.5)
    sclick(580, 495)   # close the settings page
    time.sleep(0.5)
    qk = d.keysym_to_keycode(0x71)
    deadline = time.monotonic() + 60.0
    while time.monotonic() < deadline:
        # Same popup-wedge defense as subsdrive: with a popup up, every
        # key — q included — is swallowed by the app.
        if moved(200, 400):
            holdclick(1)
        time.sleep(0.7)
        try:
            focus()
            key(qk)
        except Exception:
            sys.exit(0)
        time.sleep(0.5)
        if find_window() is None:
            sys.exit(0)
    sys.exit(4)

if mode == "substl":
    # The translate pick: same slot arithmetic as "subsdl", two picks over
    # the subtitle settings overlay's track combo. Pick 1 lands on the
    # [download] row; activating it closes the popup, so pick 2 reopens the
    # combo (the popup then lists the new #4 track and the [translate] row
    # where the [download] row was) and lands on that. Pick 2 runs read ->
    # translate -> store -> load and the selection trail then shows the
    # second external id. Coordinates probed under SOAR_UI_BITMAP_FONT=1.
    time.sleep(2.2)
    focus(); key(d.keysym_to_keycode(0x20))  # space: wake the HUD, pause
    time.sleep(0.8)

    def sclick(x, y):
        moved(x, y); time.sleep(0.25)
        holdclick(1); time.sleep(0.9)

    sclick(580, 495)   # Sub button: open the Subtitle Settings page
    time.sleep(0.9)
    sclick(459, 190)   # Track combo open: popup = Off / srt / sep / [download]
    time.sleep(1.0)
    sclick(460, 274)   # the [download] row -> fetch + store + load (id 4)
    time.sleep(2.0)    # room for the download round trip
    # Activating the [download] row also closes the popup (combo open-state
    # telemetry: the popup dies exactly at the download pick — ImGui closes
    # a combo popup when one of its rows is activated). Reopen the same
    # overlay combo; the popup now lists the new #4 track and, in place of
    # the consumed [download] row, the [translate] row at its probed rect
    # (320,288)-(616,307) (GetItemRectMin/Max under SOAR_UI_BITMAP_FONT=1).
    sclick(459, 190)   # reopen the overlay track combo
    time.sleep(1.0)
    sclick(460, 297)   # the [translate] row -> translate + store + load (id 5)
    time.sleep(3.0)    # room for the chat round trip + store + load
    sclick(200, 400)   # bare video area: dismiss any popup, toggle nothing
    time.sleep(0.5)
    sclick(580, 495)   # close the settings page
    time.sleep(0.5)
    qk = d.keysym_to_keycode(0x71)
    deadline = time.monotonic() + 60.0
    while time.monotonic() < deadline:
        # Same popup-wedge defense as subsdl: with a popup up, every
        # key — q included — is swallowed by the app.
        if moved(200, 400):
            holdclick(1)
        time.sleep(0.7)
        try:
            focus()
            key(qk)
        except Exception:
            sys.exit(0)
        time.sleep(0.5)
        if find_window() is None:
            sys.exit(0)
    sys.exit(4)

if mode == "subsembed":
    # The embedded-track [translate] pick against a chat endpoint that
    # answers 500: the pick demuxes the container's subrip stream out
    # (the POST the fixture logs proves the export ran), the translate
    # fails, and the toast arm runs. Then the harness overwrites the
    # media file under the still-open player (argv[4] played, argv[5]
    # audio-only), and the same pick fails earlier — at the export's
    # reopen probe. Neither attempt may load a track, so the selection
    # trail never leaves the container's stream range.
    time.sleep(2.2)
    focus(); key(d.keysym_to_keycode(0x20))  # space: wake the HUD, pause
    time.sleep(0.8)

    def sclick(x, y):
        moved(x, y); time.sleep(0.25)
        holdclick(1); time.sleep(0.9)

    sclick(580, 495)   # Sub button: open the Subtitle Settings page
    time.sleep(0.9)
    sclick(459, 190)   # Track combo: popup = Off / srt / sep / [translate]
    time.sleep(1.0)
    sclick(460, 274)   # the [translate] row -> export + 500 -> toast
    time.sleep(2.0)    # room for the export + POST round trip
    # Swap the media under the open player: the next export reopens the
    # path, probes an audio-only file and fails before any translation.
    if len(sys.argv) > 5:
        import shutil
        shutil.copyfile(sys.argv[5], sys.argv[4])
    time.sleep(0.5)
    sclick(459, 190)   # reopen the combo (the row is still offered: the
    time.sleep(1.0)    # failed translate left the id unrecorded)
    sclick(460, 274)   # the [translate] row again -> export-fail toast
    time.sleep(1.5)
    sclick(200, 400)   # bare video area: dismiss any popup
    time.sleep(0.5)
    sclick(580, 495)   # close the settings page
    time.sleep(0.5)
    qk = d.keysym_to_keycode(0x71)
    deadline = time.monotonic() + 60.0
    while time.monotonic() < deadline:
        # Same popup-wedge defense as subsdl: with a popup up, every
        # key — q included — is swallowed by the app.
        if moved(200, 400):
            holdclick(1)
        time.sleep(0.7)
        try:
            focus()
            key(qk)
        except Exception:
            sys.exit(0)
        time.sleep(0.5)
        if find_window() is None:
            sys.exit(0)
    sys.exit(4)

if mode == "subsdl404":
    # A dead catalog row and a live translator behind the same settings
    # combo. Pick 1 hits [download] for a URL the catalog server 404s —
    # the fetch-fail toast, nothing loaded, the row still offered on
    # reopen. Pick 2 hits [translate] on the embedded srt row: export,
    # batch chat over the ok fixture, store, load — the selection trail
    # ends on the new external id. Coordinates probed under
    # SOAR_UI_BITMAP_FONT=1 (same popup slot arithmetic as subsdl and
    # substl: the [download] row keeps its 4-row-popup slot, and the
    # [translate] row sits one selectable-plus-separator lower with both
    # candidate-list separators above it).
    time.sleep(2.2)
    focus(); key(d.keysym_to_keycode(0x20))  # space: wake the HUD, pause
    time.sleep(0.8)

    def sclick(x, y):
        moved(x, y); time.sleep(0.25)
        holdclick(1); time.sleep(0.9)

    sclick(580, 495)   # Sub button: open the Subtitle Settings page
    time.sleep(0.9)
    sclick(459, 190)   # Track combo: Off / srt / sep / [download] / sep /
    time.sleep(1.0)    # [translate]
    sclick(460, 274)   # the [download] row -> 404 -> fetch-fail toast
    time.sleep(2.0)    # room for the loopback 404 round trip
    sclick(459, 190)   # reopen the combo (the dead row is still offered)
    time.sleep(1.0)
    sclick(460, 307)   # the [translate] row -> export + chat + store + load
    time.sleep(3.0)    # room for the chat round trip + store + load
    sclick(200, 400)   # bare video area: dismiss any popup
    time.sleep(0.5)
    sclick(580, 495)   # close the settings page
    time.sleep(0.5)
    qk = d.keysym_to_keycode(0x71)
    deadline = time.monotonic() + 60.0
    while time.monotonic() < deadline:
        # Same popup-wedge defense as subsdl: with a popup up, every
        # key — q included — is swallowed by the app.
        if moved(200, 400):
            holdclick(1)
        time.sleep(0.7)
        try:
            focus()
            key(qk)
        except Exception:
            sys.exit(0)
        time.sleep(0.5)
        if find_window() is None:
            sys.exit(0)
    sys.exit(4)

if mode == "subcycle":
    # A dual-subtitle fixture whose second track was codec-patched to an
    # unregistered id (same S_TEXT/ASS -> S_TEXT/XXX patch the backend
    # suite uses). The settings combo still lists both rows; picking the
    # broken one fails at the backend (switch-failed toast), and C's
    # cycle first selects the good track, then walks onto the broken one
    # and fails the same way. The patched codec also denies the track a
    # [translate] row — the translatable scan's not-a-text-codec arm —
    # which is why the translate envs are set for this mode's run.
    time.sleep(2.2)
    focus(); key(d.keysym_to_keycode(0x20))  # space: wake the HUD, pause
    time.sleep(0.8)

    def sclick(x, y):
        moved(x, y); time.sleep(0.25)
        holdclick(1); time.sleep(0.9)

    sclick(580, 495)   # Sub button: open the page (its combo build runs
    time.sleep(0.9)    # the translatable scan over the patched codec)
    sclick(459, 190)   # Track combo: Off / srt / (patched) row 3 ...
    time.sleep(1.0)
    sclick(460, 267)   # ... the patched track row -> selectTrack fails
    time.sleep(1.0)
    sclick(200, 400)   # bare video area: dismiss the popup
    time.sleep(0.5)
    sclick(580, 495)   # close the settings page
    time.sleep(0.5)
    focus(); key(d.keysym_to_keycode(0x63))  # C: cycle selects the srt track
    time.sleep(0.6)
    focus(); key(d.keysym_to_keycode(0x63))  # C: onto the patched track ->
    time.sleep(0.6)                           # switch-failed toast
    qk = d.keysym_to_keycode(0x71)
    deadline = time.monotonic() + 60.0
    while time.monotonic() < deadline:
        if moved(200, 400):
            holdclick(1)
        time.sleep(0.7)
        try:
            focus()
            key(qk)
        except Exception:
            sys.exit(0)
        time.sleep(0.5)
        if find_window() is None:
            sys.exit(0)
    sys.exit(4)

if mode == "sidecar":
    # The sidecar-pick case over a private copy of the subs fixture: three
    # looks at the settings page's track combo. Every popup is closed by a
    # row pick or an outside click before the combo is clicked again — a
    # combo click while its own popup is up toggles the popup shut. Pass 1
    # picks the single (good) candidate row — loadExternalSubtitle +
    # selectTrack through the menu. Reopen A shows the all-loaded state:
    # the sidecar is now a real track and filtered from the candidates, so
    # the sidecar section renders nothing. The harness then stages a
    # cue-less .srt (argv[4]); reopen B offers exactly that row — one slot
    # lower than in pass 1, the loaded sidecar now rendering as a track
    # row above it — and picking it runs the load-failed toast arm.
    # Coordinates probed under SOAR_UI_BITMAP_FONT=1.
    time.sleep(2.2)
    focus(); key(d.keysym_to_keycode(0x20))  # space: wake the HUD, pause
    time.sleep(0.8)

    def sclick(x, y):
        moved(x, y); time.sleep(0.25)
        holdclick(1); time.sleep(0.9)

    sclick(580, 495)   # Sub button: open the Subtitle Settings page
    time.sleep(0.9)
    sclick(459, 190)   # Track combo: popup = Off / srt / sep / candidate
    time.sleep(1.0)
    sclick(460, 274)   # the candidate row -> load + select the external id
    time.sleep(1.0)
    # Repeat guard: if a lag frame swallowed the pick above, the popup is
    # still up and this re-picks the row. On the normal path the popup is
    # closed and this lands on the page's Sync-offset slider (same spot,
    # different context) — a setting this case never asserts on, and the
    # per-pid XDG_STATE_HOME keeps it from leaking anywhere.
    sclick(460, 274)
    time.sleep(1.0)
    sclick(459, 190)   # reopen A: sidecar loaded+filtered, no section
    time.sleep(1.0)
    sclick(200, 400)   # outside click closes the popup (video: OSC toggle)
    time.sleep(0.5)
    if len(sys.argv) > 4:
        open(sys.argv[4], "wb").close()  # stage the cue-less sibling now
    time.sleep(0.3)
    sclick(459, 190)   # reopen B: Off / srt / loaded#4 / sep / broken@290
    time.sleep(1.0)
    sclick(460, 290)   # pick it -> loadExternalSubtitle fails, toast arm
    time.sleep(1.0)
    sclick(460, 290)   # repeat guard, same as above
    time.sleep(1.0)
    sclick(200, 400)   # bare video area: dismiss any popup, toggle nothing
    time.sleep(0.5)
    sclick(580, 495)   # close the settings page
    time.sleep(0.5)
    qk = d.keysym_to_keycode(0x71)
    deadline = time.monotonic() + 60.0
    while time.monotonic() < deadline:
        # Same popup-wedge defense as subsdrive: with a popup up, every
        # key — q included — is swallowed by the app.
        if moved(200, 400):
            holdclick(1)
        time.sleep(0.7)
        try:
            focus()
            key(qk)
        except Exception:
            sys.exit(0)
        time.sleep(0.5)
        if find_window() is None:
            sys.exit(0)
    sys.exit(4)

if mode == "playlist":
    # The queue timeline (docs/mvp.md §2) over two 6s fixtures that the
    # media-info trail tells apart by track count (audio_only tracks=1,
    # subs_media tracks=3), plus a third queued path that does not exist —
    # its only role is a failed open (n onto it), which must leave the
    # queue and the current entry untouched. Keys only until the overlay
    # buttons are needed; the Playlist overlay (P) stays open the whole
    # run — overlay windows do not swallow keys, only combo popups do.
    # Coordinates probed under SOAR_UI_BITMAP_FONT=1, re-probed for v0.2.1
    # with hover-band scans and label-hash click checks on a live Xvfb:
    # the header is [Add files...][Open folder...][Loop][Shuffle][Clear
    # queue] (todo.md 原型对齐批) with the button band at y~131-148, centers
    # (262/377/482/581/683, 140) — anything above y~130 is the overlay
    # title bar, which eats clicks. The window widened 460->560, so every
    # row-anchored pin moved -50 in x. Each row's trailing "x" button
    # follows the row text (SameLine), so its x moves with the entry name
    # and with the "> " current marker (one character narrower): for these
    # siblings it sits at ~(293/302/329, 165/182/203). A click landing just
    # right of the button hits the row Selectable instead (jump semantics),
    # which is why the x is probed per row.
    # The file picker ("Add files...") opens at $HOME = the queue scratch
    # dir: rows ..//.cache//one.mkv/two.mkv at y~137/154/171/188 (the
    # recursive toggle is hidden in file mode, one pitch above the
    # watch-mode picker), checkboxes at x~240, "Add N files" bottom-left at
    # (273,459).
    # Expected media-info
    # trail, launch included, each step tagged with the gesture that
    # produced it (the two fixtures are told apart by track count, so the
    # trail needs no screenshots):
    #   1 (launch) 3 (manual n) 1 (manual Shift+N)
    #   3 (auto-advance at the first entry's EOF)
    #   1 (Loop::All wrap at the queued entry's EOF)
    #   1 (single-entry replay under Loop::One)
    #   1 (the file pick's auto-play of one.mkv — todo.md 原型对齐批)
    # The dead path is removed with an "x" click once the failed opens
    # are done, so both later EOF advances land on real entries; the
    # failed opens, the removals and the empty-queue n emit no content
    # media-info. The pick replays ~1s before its clear lands, so the
    # replay's EOF finds an emptied queue and nothing follows the pick.
    # Manual n under a loop mode wraps (next() at the tail re-opens the
    # head), which is why every n here either targets a real entry or an
    # empty queue.
    time.sleep(2.2)
    focus()
    # The beat is events-driven: the CLI tees its output to a file (the
    # caller passes its path as argv[4]) and every gesture that must land
    # inside a playback window polls that trail for the exact open
    # markers instead of trusting a hard-coded sleep budget — fixture
    # open latency and long coverage-build frames made every fixed
    # budget a coin flip (clicks routinely landed after the EOF they
    # were supposed to precede). Exit code 5 = a marker never arrived,
    # 6 = the clear's arm click already cleared; the test's own
    # assertions would flag the run anyway, this just says the injector
    # is the one that saw it.
    tee = sys.argv[4] if len(sys.argv) > 4 else ""

    def marker_count(marker):
        try:
            with open(tee) as f:
                return f.read().count(marker)
        except OSError:
            return 0

    def wait_marker(marker, want, timeout=45.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if marker_count(marker) >= want:
                return True
            time.sleep(0.25)
        return False

    n = d.keysym_to_keycode(0x6e)
    p = d.keysym_to_keycode(0x70)
    i = d.keysym_to_keycode(0x69)
    key(n)                          # n: jump to the queued entry
    time.sleep(1.0)
    key(p)                          # p: open the Playlist overlay
    time.sleep(0.8)
    key(n)                          # n onto the dead entry: open fails,
    time.sleep(0.8)                 # current entry and queue stay untouched
    key(n)                          # n again: the failed open is
    time.sleep(0.8)                 # idempotent, so a repeat only re-fails
    sh = d.keysym_to_keycode(0xFFE1)  # Shift_L
    xtest.fake_input(d, X.KeyPress, sh); d.sync()
    key(n)                          # Shift+N: back to the first entry
    xtest.fake_input(d, X.KeyRelease, sh); d.sync()
    time.sleep(0.8)
    moved(329, 203); time.sleep(0.15)
    holdclick(1); time.sleep(0.5)   # "x" on row 3 ("   missing.mp4"):
                                    # drop the dead entry from the queue
    if not wait_marker("tracks=3 selected", 2):
        sys.exit(5)                 # one EOF auto-advanced into two
    moved(482, 140); time.sleep(0.15)
    holdclick(1); time.sleep(0.5)   # Loop button: off -> all (two mid-play)
    if not wait_marker("tracks=1 selected", 3):
        sys.exit(5)                 # two EOF wrapped into one under All
    key(i)                          # i: info overlay reads "Loop all"
    time.sleep(0.9)
    key(i)                          # ... and close it (overlays are
    time.sleep(0.4)                 # single-valued, so re-open the queue)
    key(p)                          # p: Playlist overlay again
    time.sleep(0.6)
    moved(482, 140); time.sleep(0.15)
    holdclick(1); time.sleep(0.5)   # Loop button: all -> one (one mid-play)
    if not wait_marker("tracks=1 selected", 4):
        sys.exit(5)                 # one EOF replayed under One
    moved(302, 182); time.sleep(0.15)
    holdclick(1); time.sleep(0.7)   # "x" on row 2: drop the queued entry
    moved(293, 165); time.sleep(0.15)
    holdclick(1); time.sleep(0.7)   # "x" on row 1: queue is empty, hint
                                    # shows (removing the playing entry
                                    # only re-points, playback runs on)
    key(n)                          # n on an empty queue: toast, no crash
    time.sleep(0.8)
    # The two new header buttons (todo.md 原型对齐批): "Add files..." opens
    # the picker in file mode at $HOME — the scratch queue dir, so the two
    # fixtures are right there as browsable rows. The checkboxes queue in
    # click order; the queue was empty, so the first pick auto-plays and
    # the toast reads "Imported". "Clear queue" wipes in two clicks: the
    # first arms (the label flips to "Clear: sure?"), the second executes —
    # one click at one button's width from a destroyed queue is too little
    # intent for a store with no undo.
    moved(262, 140); time.sleep(0.15)
    holdclick(1); time.sleep(0.8)   # "Add files...": picker in file mode
    moved(240, 171); time.sleep(0.15)
    holdclick(1); time.sleep(0.5)   # tick one.mkv
    moved(240, 188); time.sleep(0.15)
    holdclick(1); time.sleep(0.5)   # tick two.mkv
    moved(273, 459); time.sleep(0.15)
    holdclick(1); time.sleep(0.9)   # "Add 2 files": first auto-plays
    if not wait_marker("event: toast=Imported 2 files", 1):
        sys.exit(5)                 # the file pick did not import
    moved(683, 140); time.sleep(0.15)
    holdclick(1); time.sleep(0.6)   # "Clear queue": arms only
    if marker_count("event: toast=Playlist cleared") != 0:
        sys.exit(6)                 # one click destroyed the queue
    moved(683, 140); time.sleep(0.15)
    holdclick(1); time.sleep(0.6)   # "Clear: sure?": executes
    if not wait_marker("event: toast=Playlist cleared", 1):
        sys.exit(5)                 # the confirm click did not clear
    qk = d.keysym_to_keycode(0x71)
    deadline = time.monotonic() + 60.0
    while time.monotonic() < deadline:
        # No popup is up in this mode, but keep the same safe exit shape
        # as the other modes: a click, a pause, then q until it lands.
        if moved(200, 400):
            holdclick(1)
        time.sleep(0.7)
        try:
            focus()
            key(qk)
        except Exception:
            sys.exit(0)
        time.sleep(0.5)
        if find_window() is None:
            sys.exit(0)
    sys.exit(4)

if mode == "dirpick":
    # The folder-import round trip (v0.2.1 dir picker + playlist tree) over
    # a staged scratch HOME: Ctrl+O opens the picker there, "Include
    # subfolders" flips on (load-bearing — media_a keeps one of its two
    # files a level down, so a flat import would starve the tour), a
    # subdirectory is entered and imported, and its two files queue with
    # the first auto-playing. The second Ctrl+O reopens on the persisted
    # path with a Recent folders section; every click of the second tour
    # stays on the collapsed layout (the header toggle is ImGui state —
    # racing it shifts every row below and once turned the ".." click into
    # a whole-HOME import that the trail could not tell apart), climbing
    # via ".." to HOME and importing the sibling folder mid-playback
    # (append-only, no restart). The third open exists for the section's
    # own arms — expand, recent-entry jump — placed after the imports so a
    # lagged click there cannot corrupt anything (overlays are
    # single-valued, so the P right after also dismisses the picker). P
    # shows the playlist: the queue now spans two directories, so the tree
    # view groups them; "Play group" on the second group opens its file,
    # and the first group's folder node collapses last. With the overlay
    # closed again, the wheel tour drives the scroll handler's arms over
    # the clear video area (volume up/down, Shift+wheel seek, horizontal
    # seek — all silent, none of them print a marker). Then the quit
    # storm ends the run.
    #
    # Coordinates probed under SOAR_UI_BITMAP_FONT=1 on a live Xvfb
    # (hover-band scans, label-hash click checks, once against the -O0
    # coverage build's slow frames): the picker is a ~520x420 window at
    # (220,62) — checkbox (243,126), ".." (240,160), subdirectory rows
    # from y~177 with ~17.5 spacing (the staged HOME pre-creates a
    # ".cache" sibling, and a dot-directory always sorts first, so
    # "media_a"/"media_b" always land at ~192/~212 whatever the app itself
    # drops into HOME), Import bottom-left at (359,459) (left-anchored, so
    # the label-length difference between the two imports is free). On a
    # reopen the collapsed Recent folders header sits at (240,162) and
    # pushes the rows to ".." ~188, .cache ~205, media_a ~223, media_b
    # ~243; expanded, its first entry is at (259,187) regardless of entry
    # count. In the playlist tree view the group headers are at y~164
    # (media_a) and ~222 (media_b) with "Play group" at x~378, member rows
    # ~20.5 below their header; clicking the folder-node label (260,164)
    # collapses the group. (The tree lives in the playlist window, which
    # the add/clear batch widened 460->560: centered, so every x here
    # shifted -50 from the first probes.)
    # The trail tells the fixtures apart by track count, so every step is
    # asserted from the teed media-info lines alone:
    #   1 (recursive import auto-plays a1) 3 (a1 EOF advanced into the
    #   deeper a2 — both files really queued) 1 (media_b "Play group"
    #   opened b1). The waits gate the gestures: the second import must
    #   land while a player is up (the append branch), and the "Play
    #   group" click must beat a2's own EOF (it does — the click runs
    #   ~2.5s after the a2-open marker, a2 runs 6s).
    time.sleep(2.2)
    focus()
    tee = sys.argv[4] if len(sys.argv) > 4 else ""

    def marker_count(marker):
        try:
            with open(tee) as f:
                return f.read().count(marker)
        except OSError:
            return 0

    def wait_marker(marker, want, timeout=45.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if marker_count(marker) >= want:
                return True
            time.sleep(0.25)
        return False

    ctrl = d.keysym_to_keycode(0xffe3)  # Control_L

    def ctrl_key(ch):
        # Hold Ctrl across the letter: a combo only registers when the
        # modifier and the key straddle a frame boundary, the same race
        # holdclick() solves for mouse clicks.
        if not focus():
            sys.exit(4)
        xtest.fake_input(d, X.KeyPress, ctrl)
        d.sync()
        time.sleep(0.06)
        key(d.keysym_to_keycode(ord(ch)))
        time.sleep(0.06)
        xtest.fake_input(d, X.KeyRelease, ctrl)
        d.sync()
        time.sleep(0.9)

    def pick(x, y):
        moved(x, y); time.sleep(0.15)
        holdclick(1); time.sleep(0.5)

    ctrl_key('o')                    # picker at the staged HOME (no recents)
    pick(243, 126)                   # "Include subfolders" on — the a2
                                     # marker below is the proof it took
    pick(259, 192)                   # enter media_a/
    pick(359, 459)                   # Import: 2 files queue, a1 auto-plays
    if not wait_marker("tracks=1 selected", 1):
        sys.exit(5)
    ctrl_key('o')                    # reopen on media_a, section collapsed
    pick(240, 188)                   # ".." climbs to the staged HOME
    pick(259, 243)                   # enter media_b/ (collapsed rows)
    pick(359, 459)                   # Import mid-playback: appends only
    if not wait_marker("tracks=3 selected", 1):
        sys.exit(5)                  # a1 EOF advanced into a2 (both queued)
    ctrl_key('o')                    # third open at media_b: 2 recents
    pick(240, 162)                   # expand the Recent folders section
    pick(259, 187)                   # its first entry (media_b, already
                                     # current — jump arm, no-op here)
    key(d.keysym_to_keycode(0x70))   # P: two directories -> tree view
    time.sleep(0.9)
    pick(378, 222)                   # media_b "Play group": b1 opens
    if not wait_marker("tracks=1 selected", 2):
        sys.exit(5)
    pick(260, 164)                   # collapse the media_a folder node
    time.sleep(0.8)

    # Wheel dial tour (IINA scroll-action defaults): the playlist overlay
    # still covers the video, so close it first — with an ImGui window
    # under the pointer the events die in the WantCaptureMouse early
    # return instead of reaching the volume/seek handler. Then, over the
    # clear video area: buttons 4/5 are volume up/down, Shift+wheel is
    # the seek variant, 6/7 the horizontal one — every arm of the
    # SDL_MOUSEWHEEL case in one pass. The seeks may run b1 to its end;
    # the tour's markers are all consumed by then and EOF stops quietly.
    key(d.keysym_to_keycode(0x70))   # P: playlist overlay closed again
    time.sleep(0.9)
    if not focus():
        sys.exit(4)
    moved(480, 300); time.sleep(0.4)
    sh = d.keysym_to_keycode(0xffe1)
    for btn, shift in ((4, False), (5, False), (4, True), (5, True),
                       (6, False), (7, False)):
        if shift:
            xtest.fake_input(d, X.KeyPress, sh); d.sync(); time.sleep(0.06)
        xtest.fake_input(d, X.ButtonPress, btn); d.sync()
        time.sleep(0.15)
        xtest.fake_input(d, X.ButtonRelease, btn); d.sync()
        if shift:
            xtest.fake_input(d, X.KeyRelease, sh); d.sync()
        time.sleep(0.45)

    qk = d.keysym_to_keycode(0x71)
    deadline = time.monotonic() + 60.0
    while time.monotonic() < deadline:
        if moved(200, 400):
            holdclick(1)
        time.sleep(0.7)
        try:
            focus()
            key(qk)
        except Exception:
            sys.exit(0)
        time.sleep(0.5)
        if find_window() is None:
            sys.exit(0)
    sys.exit(4)

if mode == "library":
    # The media-library round trip (docs/mvp.md §3): B opens the overlay,
    # "Add folder..." starts the picker in watch mode (no subfolders toggle
    # — a library watches a tree), the media_a row enters it, and "Watch
    # this folder" records + scans + saves, landing back on the Library
    # overlay with the folder and its two files. Click order is
    # load-bearing: a row click navigates, only the bottom button watches
    # the ENTERED folder. The sidebar buckets then take over: Resume and
    # Finished must not leak entries (a click aimed at an empty slot is
    # asserted harmless from the store). The search box takes the first
    # pitch of the entries column: "/" focuses it, typed keys narrow the
    # bucket's rows, and the needle composes with the bucket — Recent plus
    # "a" still lists both files in Recent order (a2 first, its file is an
    # hour newer), so row 1's "x" forgets exactly a2 and a1 surviving is
    # the filter-leak tripwire.
    # Coordinates probed under SOAR_UI_BITMAP_FONT=1 on a 1280x800 Xvfb
    # against the -O0 coverage build, in the injector's own frame: the
    # SDL window is 960x540 centred at (160,130), and moved() warps
    # window-relative. The overlay is 520x420 centred in the window —
    # "Add folder..." at (287,100), "Rescan" at (375,100); the watch-mode
    # picker (checkbox hidden, rows one pitch higher than the import
    # layout) shows ".." ~127, ".cache/" ~144, "media_a/" ~161, with
    # "Watch this folder (2 media files, with subfolders)" bottom-left
    # anchored at (418,459). Below the folder block sits the browse
    # split: bucket rows All/Recent/Resume/Finished at x~280,
    # y~163/181/200/219; the entries column starts with the search box
    # (its hint text spans y~164-174), so entry rows sit one pitch lower
    # than the buckets — row 1 y~194, row 2 y~213 — with the row's "x"
    # right-aligned to x~716 whatever the label's length. argv[4] is the
    # store path; every destructive click is confirmed against it before
    # the next step.
    time.sleep(2.2)
    focus()
    store = sys.argv[4] if len(sys.argv) > 4 else ""

    def store_txt():
        if not store:
            return ""
        try:
            with open(store) as f:
                return f.read()
        except OSError:
            return ""

    def has_folder_line():
        return any(line.startswith("f ") for line in store_txt().splitlines())

    def sclick(x, y):
        moved(x, y); time.sleep(0.15)
        holdclick(1); time.sleep(0.5)

    key(d.keysym_to_keycode(0x62))  # B -> the Library overlay
    time.sleep(0.9)
    sclick(287, 100)   # "Add folder..." -> the picker in watch mode
    time.sleep(0.9)
    sclick(250, 161)   # enter media_a/
    time.sleep(0.7)
    sclick(418, 459)   # "Watch this folder" -> record + scan + save
    deadline = time.monotonic() + 8.0
    while time.monotonic() < deadline and not has_folder_line():
        time.sleep(0.25)

    def wait_no(match, timeout=8.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if match not in store_txt():
                return True
            time.sleep(0.25)
        return False

    # The folder row's "x" sits after the row label, and the label is the
    # watched path itself — pid-dependent in length. Compute the x from
    # the label (text starts at ~233, ~7 px/char in the bitmap font, plus
    # the SameLine gap and half a small button ~15) instead of pinning a
    # coordinate that only fits one pid width.
    home = sys.argv[5] if len(sys.argv) > 5 else ""
    folder_x = 233 + len(home + "/media_a") * 7 + 15

    # Back on the Library overlay: park the pointer on the folder row,
    # then a1's row (row 1 of All; its tooltip shows path + duration +
    # play count), then Rescan.
    moved(245, 136)
    time.sleep(0.8)
    moved(480, 194)
    time.sleep(0.8)
    sclick(375, 100)   # "Rescan" -> scan + save again (store unchanged)
    time.sleep(0.6)

    # Resume holds only a1 (the seeded 5:21 of a 10-minute file): a click
    # at the second slot must be a no-op — a2 vanishing here means the
    # bucket filter leaks (exit 6).
    sclick(280, 200)   # the Resume bucket
    time.sleep(0.5)
    sclick(716, 213)   # second slot is empty under Resume
    time.sleep(0.5)
    if store and "a2.mkv" not in store_txt():
        sys.exit(6)

    # Finished holds nothing (nothing reached the end): the first slot
    # must be empty too.
    sclick(280, 219)   # the Finished bucket
    time.sleep(0.5)
    sclick(716, 194)
    time.sleep(0.5)
    if store and "a1.mkv" not in store_txt():
        sys.exit(6)

    # The search half (docs/todo.md 原型对齐批): the Recent bucket, then
    # "/" focuses the box and a typed needle narrows the bucket's rows.
    # The needle "a" matches both file paths, so the list keeps the
    # bucket's order — and Recent's scan tokens put the hour-newer a2
    # first: row 1's "x" must forget exactly a2, with a1 surviving as the
    # proof the filter did not leak past the needle.
    sclick(280, 181)   # the Recent bucket
    time.sleep(0.5)
    key(d.keysym_to_keycode(0x2f))  # / -> focus the search box
    time.sleep(0.5)
    key(d.keysym_to_keycode(0x61))  # a
    time.sleep(0.6)
    sclick(716, 194)   # row 1 = a2's "x" (Recent order under the needle)
    if store and not wait_no("a2.mkv"):
        sys.exit(7)
    if store and "a1.mkv" not in store_txt():
        sys.exit(8)

    # Clear the needle (the box is refocused by the same "/") so the
    # remaining steps see the buckets unfiltered: a stale needle would
    # empty the next slot and turn its "x" click into a no-op the final
    # wait would catch.
    key(d.keysym_to_keycode(0x2f))  # / -> refocus the search box
    time.sleep(0.5)
    key(d.keysym_to_keycode(0xff08))  # backspace clears the lone "a"
    time.sleep(0.6)

    # Recent keeps only a1 now; the second slot must be an empty no-op —
    # a1 vanishing means a stale row landed where nothing should be.
    sclick(280, 181)   # the Recent bucket
    time.sleep(0.5)
    sclick(716, 213)
    time.sleep(0.5)
    if store and "a1.mkv" not in store_txt():
        sys.exit(6)

    sclick(folder_x, 136)   # folder row's "x" -> removeFolder + save
    wait_no("\nf ")
    time.sleep(0.4)
    qk = d.keysym_to_keycode(0x71)
    deadline = time.monotonic() + 30.0
    while time.monotonic() < deadline:
        if moved(200, 400):
            holdclick(1)
        time.sleep(0.7)
        try:
            focus()
            key(qk)
        except Exception:
            sys.exit(0)
        time.sleep(0.5)
        if find_window() is None:
            sys.exit(0)
    sys.exit(4)

if mode == "audiodrive":
    # Real-backend window run over the dual-audio fixture: two audio
    # streams and no subtitle stream at all. That combination is where
    # the "nothing to cycle" paths live -- A steps to the second stream
    # and the next press wraps back to the first, C has no subtitle to
    # cycle, the Subtitle Settings page says so instead of offering an
    # empty combo, and Media Info lists both titled audio rows with one
    # of them marked selected. The null-backend tour has a single
    # synthetic track, so none of these arms are reachable there.
    time.sleep(2.2)
    focus(); key(d.keysym_to_keycode(0x20))  # space: hold the decode thread
    time.sleep(0.6)                          # alive for the switch handoff

    def dclick(x, y):
        moved(x, y); time.sleep(0.15)
        button(1); time.sleep(0.5)

    for keysym in (0x61, 0x61, 0x63):  # a -> second stream, a -> wrap, c -> none
        if not focus():
            sys.exit(4)
        key(d.keysym_to_keycode(keysym))
        time.sleep(0.6)

    # (Pins re-probed with the seek-row clocks: the old 637/685 pair
    # had drifted onto the subtitle combo / Sub button — tolerated only
    # because nothing downstream asserted on the pages.)
    dclick(580, 495)   # Sub page: "No subtitle tracks available"
    dclick(580, 495)   # close it again
    dclick(630, 495)   # Media Info: two titled audio rows, one selected
    dclick(630, 495)   # close it again
    time.sleep(0.4)
    qk = d.keysym_to_keycode(0x71)
    deadline = time.monotonic() + 60.0
    while time.monotonic() < deadline:
        # Same popup-wedge defense as subsdrive: the clicks above leave a
        # combo open when the instrumented app lags, and a stuck popup
        # swallows the q storm below.
        if moved(200, 400):
            holdclick(1)
        time.sleep(0.7)
        try:
            focus()
            key(qk)
        except Exception:
            sys.exit(0)
        time.sleep(0.5)
        if find_window() is None:
            sys.exit(0)
    sys.exit(4)

if mode == "assdrive":
    # Real-backend window run over the styled ASS fixture, rendered through
    # the libass overlay. No HUD driving: the point is the video-present
    # path, so the tour is a V round-trip (batch 1c: the toggle gates the
    # canvas overlay off and on, both arms of the blend check) and a
    # fullscreen round-trip — the destination rectangle changes, the
    # overlay texture is recreated for the new geometry, and Escape back
    # restores it — followed by the standard escape storm. The fixture's
    # two cues span its whole 4s, so overlay work happens throughout.
    time.sleep(2.0)
    focus()
    key(d.keysym_to_keycode(0x76))  # v: canvas off (toggle hides subtitles)
    time.sleep(0.4)
    key(d.keysym_to_keycode(0x76))  # v: canvas back on
    time.sleep(0.4)
    key(d.keysym_to_keycode(0x66))  # f: fullscreen (recreate at the new size)
    time.sleep(1.6)
    key(d.keysym_to_keycode(0x66))  # f: back to windowed (recreate again)
    time.sleep(0.8)
    ek = d.keysym_to_keycode(0xFF1B)
    deadline = time.monotonic() + 60.0
    while time.monotonic() < deadline:
        try:
            focus()
            key(ek)
        except Exception:
            sys.exit(0)
        time.sleep(0.5)
        if find_window() is None:
            sys.exit(0)
    sys.exit(4)

if mode == "stall":
    # The window's buffering indicator is the one HUD element driven by a
    # backend event rather than by input: the UI polls the atomic that
    # main.cpp sets from BufferingStarted/Ended. Sit still through the
    # whole throttled arc — the mid-body pause crosses the backend's 10s
    # stall threshold (BufferingStarted), and the server's second burst
    # ~40s in ends it (BufferingEnded, the print half no other case
    # reaches) — then quit. No clicking: the HUD is allowed to auto-hide,
    # which is the point (the chip is not part of the OSC).
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
        # S: this fixture is audio-only, so no frame was ever presented —
        # the screenshot fails with "no video frame" and the HUD reports
        # it instead of writing a file (saveScreenshot's false path).
        s = d.keysym_to_keycode(0x73)
        xtest.fake_input(d, X.KeyPress, s)
        d.sync()
        xtest.fake_input(d, X.KeyRelease, s)
        d.sync()
    except Exception:
        pass
    time.sleep(46)
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
    # download arc. The Media Info detour is the only place the HUD
    # prints the cache directory it is filling. Quit with Escape, not Q:
    # the bare-SDL window (the no-imgui build) only handles Escape, and
    # the Q injector loop once spun to the deadline there (no-imgui CI
    # job, 92f5c6b).
    time.sleep(4)
    try:
        focus()
        key(d.keysym_to_keycode(0x69))  # I: Media Info, the Cache: row
        time.sleep(0.8)
        focus()
        key(d.keysym_to_keycode(0x69))  # ... and close it again
    except Exception:
        pass
    time.sleep(4)
    qk = d.keysym_to_keycode(0xFF1B)  # XK_Escape
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
# An ImGui combo popup left open by lagged clicks owns the keyboard —
# the app's popup guard swallows every shortcut, Escape included, so the
# quit storm below would spin until the injector's deadline. One click
# on the bare video area (single click only toggles the OSC) dismisses
# any popup first.
if moved(200, 400):
    button(1)
    time.sleep(0.5)
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

RunResult runCli(const std::vector<std::string>& args, int child_timeout_secs = 600,
                const std::string& tee_path = {}) {
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
  // Coverage builds (--coverage -O0 -g) are significantly slower; allow
  // up to 600s by default so heavy UI-scripted cases don't time out under
  // instrumentation. Cases whose child outlives even that (measured, see
  // the call sites) pass a bigger budget instead of everyone sharing one.
  // BUGS.md #4: the kill signal is ABRT, not TERM — a hung child carries
  // the in-binary hang dump (SOAR_HANG_DUMP, installed in soarAppMain)
  // that prints the stuck thread's backtrace before dying. timeout still
  // exits 124 either way, so the retry-on-124 logic is unchanged;
  // --kill-after escalates to KILL only if a child ever survives ABRT.
  // The probe rides every Linux child via the prefix (2026-10-16: the
  // library overlay walkthrough hung to a bare core dump, run
  // 38792276031 — arming had stayed case-scoped and the newer
  // walkthroughs never picked it up); the headless helpers' own
  // ScopedEnv sites become redundant but harmless. Under TSan the
  // handler's own backtrace malloc draws a "signal-unsafe call inside
  // of a signal" WARNING on the instrumented child (run 38803623890) —
  // expected noise, not a new race: the dump still prints and the
  // watchdog still owns the exit code (signal-unsafe is warning-class,
  // halt_on_error only stops error-class reports). Keep it armed there
  // too: the sanitizer legs are where the 124 family lives, and this
  // run's stack — Player::stop() stuck in thread join (player.cpp:46)
  // — is the first hard location for BUGS.md #4.
  cmd.insert(0, "SOAR_HANG_DUMP=1 timeout -s ABRT --kill-after=10 " +
                    std::to_string(child_timeout_secs) + " ");
#endif

  RunResult result;
  FILE* pipe = SOAR_POPEN(cmd.c_str(), "r");
  REQUIRE(pipe != nullptr);
  // Optional tee: the same captured bytes, flushed line-by-line to a file
  // so a concurrently running injector (the X11 playlist walkthrough)
  // can poll the CLI's event trail and drive its clicks off observed
  // playback state instead of a fixed sleep budget — open latency and
  // long coverage frames made every hard-coded budget a coin flip.
  FILE* tee = tee_path.empty() ? nullptr : std::fopen(tee_path.c_str(), "w");
  char buffer[512];
  // fgets, not fread: fread loops until the full request or EOF, so a child
  // that goes quiet strands its tail bytes inside the pipe and the tee file
  // freezes mid-trail. The injector polls that file — a toast sitting in the
  // stranded tail (the playlist clear's confirmation) never reached it, the
  // quit storm never fired, and the run burned the whole 600s budget on an
  // app that was actually done. fgets returns on every newline, so the file
  // stays current line-by-line as the comment above promises.
  while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    result.output.append(buffer);
    if (tee != nullptr) {
      std::fputs(buffer, tee);
      std::fflush(tee);
    }
  }
  if (tee != nullptr) std::fclose(tee);

#ifdef _WIN32
  result.exit_code = SOAR_PCLOSE(pipe);
#else
  const int status = SOAR_PCLOSE(pipe);
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
  return result;
}

// BUGS.md #4: the retry-on-124 in the streaming helpers below keeps the
// leg green when only one child times out — and the in-binary hang dump
// (SOAR_HANG_DUMP) rides the timed-out child's captured output, which the
// retry discards. The tail printed at the retry site puts a single caught
// hang's backtrace into the run log; the common (exit 0) path never calls
// it. Bounded so a child that hung mid-download cannot flood the log.
std::string hangDumpTail(const std::string& output, std::size_t max_bytes) {
  if (output.size() <= max_bytes) {
    return output;
  }
  return output.substr(output.size() - max_bytes);
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

  // 180s per child: these streams end in single-digit seconds on CI and
  // stay far under it under coverage instrumentation. The default 600 is
  // sized for the UI-scripted cases, not needed here.
  // BUGS.md #4: arm the in-binary hang dump so a watchdog-killed child
  // prints its stack instead of dying silently.
  const ScopedEnv hang_dump("SOAR_HANG_DUMP", "1");
  auto play = [&]() {
    return runCli({"--headless", "--backend=ffmpeg",
                   "http://127.0.0.1:" + port + url_path}, 180);
  };
  result.cli = play();
  // BUGS.md #4: a finished playback can intermittently fail to exit (seen
  // on CI after `event: state=3`/Ended, the event loop already broken),
  // which the child watchdog turns into exit 124. Retry once while the
  // fixture server is still up: the hang is per-process and rare, so a
  // second child exiting 0 passes; a case that hangs every time still
  // fails on the second 124. Print the first child's tail first and
  // flush: a passing retry would otherwise keep the dump hidden in the
  // discarded capture, and an unflushed buffer is lost if the retry child
  // hangs the suite into a ctest kill (the p2p suite's line-buffering
  // lesson).
  if (result.cli.exit_code == 124) {
    std::fprintf(stdout,
                 "BUGS.md #4: first child timed out; its hang dump tail, "
                 "then one retry:\n%s\n",
                 hangDumpTail(result.cli.output, 4096).c_str());
    std::fflush(stdout);
    result.cli = play();
  }
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

  // 180s per child and the BUGS.md #4 armor, same reasoning as the HTTP
  // variant: the hang family reached this case on the tsan leg (2026-10-14,
  // exit 124 with no stack because this helper armed no probe). Arm the
  // in-binary hang dump and retry once — the hang is per-process and rare,
  // so a second child exiting 0 passes; a case that hangs every time still
  // fails on the second 124.
  const ScopedEnv hang_dump("SOAR_HANG_DUMP", "1");
  auto play = [&]() {
    return runCli({"--headless", "--backend=ffmpeg",
                   "rtsp://127.0.0.1:" + port + "/live"}, 180);
  };
  result.cli = play();
  if (result.cli.exit_code == 124) {
    std::fprintf(stdout,
                 "BUGS.md #4: first child timed out; its hang dump tail, "
                 "then one retry:\n%s\n",
                 hangDumpTail(result.cli.output, 4096).c_str());
    std::fflush(stdout);
    result.cli = play();
  }
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

TEST_CASE("--help prints usage and exits with 0") {
  // Help is usage-class output (expected behavior, exit 0), distinct
  // from the no-source error exit.
  const auto run = runCli({"--help"});
  CHECK(run.exit_code == 0);
  CHECK(run.output.find("Usage:") != std::string::npos);
  const auto short_run = runCli({"-h"});
  CHECK(short_run.exit_code == 0);
  CHECK(short_run.output.find("Usage:") != std::string::npos);
}

TEST_CASE("--gui with --headless and no source keeps the usage exit") {
  // --gui only re-routes the windowed no-source start; combined with
  // --headless there is still nothing to do, so the console contract
  // holds (BUGS.md #3 guards this corner explicitly).
  const auto run = runCli({"--gui", "--headless"});
  CHECK(run.exit_code == 2);
  CHECK(run.output.find("Usage:") != std::string::npos);
}

TEST_CASE("unknown backend name exits with 2") {
  const auto run = runCli({"--backend=walrus", "asset://sample"});
  CHECK(run.exit_code == 2);
  CHECK(run.output.find("Unknown backend type: walrus") != std::string::npos);
  CHECK(run.output.find("Available backends: null") != std::string::npos);
}

TEST_CASE("unknown hwdec name exits with 2") {
  // The CLI rejects typos at launch: the backend would silently decode in
  // software, and an unnoticed fallback is exactly what the flag exists to
  // avoid.
  const auto run = runCli({"--hwdec=bogus", "asset://sample"});
  CHECK(run.exit_code == 2);
  CHECK(run.output.find("Unknown hwdec type: bogus") != std::string::npos);
  CHECK(run.output.find("Available hwdec types:") != std::string::npos);
}

TEST_CASE("--hwdec auto with the null backend keeps the headless flow green") {
  // The null backend ignores hwdec (MediaSource carries it, the backend
  // doesn't act on it); the CLI must still accept the flag on that path.
  const auto run =
      runCli({"--headless", "--backend=null", "--hwdec=auto", "asset://sample"});
  CHECK(run.exit_code == 0);
  CHECK(run.output.find("=== Media Info ===") != std::string::npos);
}

TEST_CASE("headless FFmpeg run with --hwdec=auto falls back to software") {
  // On GPU-less CI the platform hwaccel devices (vaapi et al.) cannot
  // open, so --hwdec=auto must degrade cleanly to the software decoder
  // and play the fixture through — the hardware path itself is only
  // observable on real hardware (docs/mvp.md §3).
  std::string media;
  if (!envMediaPath("SOAR_TEST_MEDIA", media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping");
    return;
  }
  const auto run = runCli({"--headless", "--backend=ffmpeg", "--hwdec=auto", media});
  if (run.output.find("Falling back to null backend") != std::string::npos) {
    MESSAGE("FFmpeg support not compiled in; skipping");
    return;
  }
  CHECK(run.exit_code == 0);
  CHECK(run.output.find("=== Media Info ===") != std::string::npos);
  CHECK(run.output.find("Error:") == std::string::npos);
}

TEST_CASE("every named hwdec type opens headless and degrades to software") {
  // GPU-less CI cannot open any hwaccel device, so every supported name
  // must take the clean software fallback and play the fixture through —
  // this drives the device mapping and the fallback loop for each value
  // (docs/mvp.md §3). The armed-decoder path itself is driven by the
  // SOAR_TEST_HWACCEL_ARM case below.
  std::string media;
  if (!envMediaPath("SOAR_TEST_MEDIA", media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping");
    return;
  }
  const std::vector<std::string> names = {
      "none",  "auto",  "vaapi",        "vdpau",      "d3d11",
      "d3d11va", "dxva2", "videotoolbox", "cuda",     "nvdec",
      "VAAPI", "Auto"};  // mixed case exercises the lowercasing too
  for (const std::string& name : names) {
    CAPTURE(name);
    const auto run =
        runCli({"--headless", "--backend=ffmpeg", "--hwdec=" + name, media});
    if (run.output.find("Falling back to null backend") != std::string::npos) {
      MESSAGE("FFmpeg support not compiled in; skipping");
      return;
    }
    CHECK(run.exit_code == 0);
    CHECK(run.output.find("=== Media Info ===") != std::string::npos);
    CHECK(run.output.find("Error:") == std::string::npos);
  }
}

TEST_CASE("test hwaccel arm drives the armed decoder path end to end") {
  // SOAR_TEST_HWACCEL_ARM=1 stands in for the device create: the decoder
  // arms with a device reference on its own first software pixel format,
  // libavcodec runs the real get_format negotiation, and decodeLoop takes
  // the hardware-frame download branch — where the transfer correctly
  // fails (a software frame has no hw frames context) and the frame is
  // dropped, keeping the run otherwise green. Only the transfer itself
  // needs real hardware (docs/mvp.md §3). Close releases the armed device
  // reference.
  std::string media;
  if (!envMediaPath("SOAR_TEST_MEDIA", media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping");
    return;
  }
  ScopedEnv arm("SOAR_TEST_HWACCEL_ARM", "1");
  const auto run = runCli({"--headless", "--backend=ffmpeg", "--hwdec=auto", media});
  if (run.output.find("Falling back to null backend") != std::string::npos) {
    MESSAGE("FFmpeg support not compiled in; skipping");
    return;
  }
  CHECK(run.exit_code == 0);
  CHECK(run.output.find("=== Media Info ===") != std::string::npos);
  CHECK(run.output.find("Error:") == std::string::npos);
}

TEST_CASE("--hwdec=none keeps the source on the software decoder even when armed") {
  // The arm seam below sets a device reference up for every open; "none"
  // must still gate the whole hwaccel path off (docs/mvp.md §3: the opt-out
  // outranks the request).
  std::string media;
  if (!envMediaPath("SOAR_TEST_MEDIA", media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping");
    return;
  }
  ScopedEnv arm("SOAR_TEST_HWACCEL_ARM", "0");
  const auto run =
      runCli({"--headless", "--backend=ffmpeg", "--hwdec=none", media});
  if (run.output.find("Falling back to null backend") != std::string::npos) {
    MESSAGE("FFmpeg support not compiled in; skipping");
    return;
  }
  CHECK(run.exit_code == 0);
  CHECK(run.output.find("=== Media Info ===") != std::string::npos);
  CHECK(run.output.find("Error:") == std::string::npos);
}

TEST_CASE("an empty SOAR_TEST_HWACCEL_ARM behaves as unset") {
  // Guards the seam's own parse: an exported-but-empty variable must not
  // arm anything (atoi("") would read as the software pixel format and
  // silently arm every open).
  std::string media;
  if (!envMediaPath("SOAR_TEST_MEDIA", media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping");
    return;
  }
  ScopedEnv arm("SOAR_TEST_HWACCEL_ARM", "");
  const auto run =
      runCli({"--headless", "--backend=ffmpeg", "--hwdec=auto", media});
  if (run.output.find("Falling back to null backend") != std::string::npos) {
    MESSAGE("FFmpeg support not compiled in; skipping");
    return;
  }
  CHECK(run.exit_code == 0);
  CHECK(run.output.find("=== Media Info ===") != std::string::npos);
  CHECK(run.output.find("Error:") == std::string::npos);
}

TEST_CASE("--version prints the build version and exits with 0") {
  const auto run = runCli({"--version"});
  CHECK(run.exit_code == 0);
  CHECK(run.output.find("soar ") != std::string::npos);
}

TEST_CASE("a malformed magnet fails fast with the torrent parse error") {
  // No xt= parameter: the parse half of the torrent start rejects the URI
  // before any session or swarm setup, so this stays a millisecond-scale
  // failure (not the 60s metadata wait of a well-formed hash).
  const auto run = runCli({"--headless", "magnet:?dn=broken"});
  CHECK(run.exit_code == 1);
  CHECK(run.output.find("torrent: cannot parse magnet URI") != std::string::npos);
}

TEST_CASE("a missing .torrent file fails fast with the torrent parse error") {
  // The file-extension arm of the positional check, and the default store
  // (temp directory) branch the flags case below skips.
  const auto run = runCli({"--headless", "/nonexistent/soar-nope.torrent"});
  CHECK(run.exit_code == 1);
  CHECK(run.output.find("torrent: cannot parse /nonexistent/soar-nope.torrent") !=
        std::string::npos);
}

TEST_CASE("torrent flags reach the stream params before the parse fails") {
#ifdef _WIN32
  // MSVC has no POSIX mkdtemp; the flags land in the params regardless of
  // where the store points, so the Linux case below is the assertion.
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  // --torrent-store skips the temp-dir default, --torrent-peer and
  // --torrent-index land in the params, --torrent-list sets the listing
  // flag — all observable through one malformed magnet that still fails
  // fast afterwards.
  std::string tmpl = "/tmp/soar_cli_torrent_XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  const char* tmp = ::mkdtemp(buf.data());
  REQUIRE(tmp != nullptr);
  const auto run = runCli({"--headless",
                           std::string("--torrent-store=") + tmp,
                           "--torrent-peer=127.0.0.1:1",
                           "--torrent-index=0",
                           "--torrent-list",
                           "magnet:?dn=broken"});
  CHECK(run.exit_code == 1);
  CHECK(run.output.find("torrent: cannot parse magnet URI") != std::string::npos);
#endif
}

TEST_CASE("a malformed magnet in window mode fails before the window opens") {
  // Window mode takes the startAsync arm: the synchronous prepare half
  // still rejects the URI, so the run exits 1 without ever reaching SDL.
  const auto run = runCli({"magnet:?dn=broken"});
  CHECK(run.exit_code == 1);
  CHECK(run.output.find("torrent: cannot parse magnet URI") != std::string::npos);
}

#if defined(SOAR_SEED_TORRENT)
// The seeder harness is POSIX-only (fork/execlp/waitpid, like the RTSP
// helper below); the two seeded-torrent cases skip on Windows before they
// reach it.
#ifndef _WIN32
namespace {

// Minimal DLNA/UPnP MediaRenderer for the --cast cases (docs/mvp.md §3):
// a device description with an AVTransport service whose controlURL
// accepts the four SOAP actions the control point sends, a position clock
// that advances so the session's stall detector sees a playing receiver,
// and — the point of the whole exercise — a fetch of the CurrentURI after
// SetAVTransportURI, exactly what a real DMR does with the URL it is
// handed. Every observed event goes to the log file as one line:
//   seturi <url> | fetch <status> <bytes> | fetch-fail <reason> | play | stop
constexpr char kDlnaRendererScript[] = R"PY(
import re, socket, sys, threading, time, urllib.request

port = int(sys.argv[1])
log_path = sys.argv[2]
mode = sys.argv[3]  # "ok" has AVTransport; "noav" only RenderingControl;
                    # "ssdp" is "ok" plus the discovery responder

SSDP_GROUP = "239.255.255.250"
SSDP_PORT = 1900
AVT = "urn:schemas-upnp-org:service:AVTransport:1"

def log(line):
    with open(log_path, "a") as f:
        f.write(line + "\n")

SERVICE = (AVT if mode != "noav"
           else 'urn:schemas-upnp-org:service:RenderingControl:1')
DESC = ('<?xml version="1.0" encoding="utf-8"?>'
        '<root xmlns="urn:schemas-upnp-org:device-1-0">'
        '<device>'
        '<deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>'
        '<friendlyName>Test Renderer</friendlyName>'
        '<UDN>uuid:soar-test-renderer</UDN>'
        '<serviceList><service>'
        '<serviceType>' + SERVICE + '</serviceType>'
        '<serviceId>urn:upnp-org:serviceId:AVT</serviceId>'
        '<controlURL>/ctl</controlURL>'
        '<eventSubURL>/evt</eventSubURL>'
        '<SCPDURL>/scpd.xml</SCPDURL>'
        '</service></serviceList>'
        '</device></root>')

def reply(payload):
    return ("HTTP/1.1 200 OK\r\nContent-Type: text/xml\r\nContent-Length: "
            + str(len(payload)) + "\r\nConnection: close\r\n\r\n" + payload)

def action_ok(action):
    return ('<?xml version="1.0"?><s:Envelope '
            'xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body>'
            '<u:' + action + 'Response '
            'xmlns:u="urn:schemas-upnp-org:service:AVTransport:1"/>'
            '</s:Body></s:Envelope>')

def ssdp_responder(up, down):
    # Answers the control point's M-SEARCH bursts on the SSDP group so the
    # discovery half of --cast=auto runs against this fixture's device.
    # The reply is unicast to the requester's source port: the control
    # point listens on an ephemeral port, not the group port. It answers
    # at once rather than honouring MX, so our fixture wins found[0] over
    # any real device that respects the MX delay.
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        if hasattr(socket, "SO_REUSEPORT"):
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        s.bind(("", SSDP_PORT))
        # Join on the default interface: the control point sends without
        # pinning one, so the answer must come from the same interface.
        s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                     socket.inet_aton(SSDP_GROUP) + socket.inet_aton("0.0.0.0"))
        s.settimeout(0.5)
        up.set()
        while True:
            try:
                data, addr = s.recvfrom(4096)
            except socket.timeout:
                continue
            except OSError:
                return
            head = data.decode("iso-8859-1", "replace")
            if not head.startswith("M-SEARCH"):
                continue
            st = ""
            for line in head.split("\r\n")[1:]:
                if line[:3].upper() == "ST:":
                    st = line.split(":", 1)[1].strip()
            if st != AVT and st != "ssdp:all" and st != "upnp:rootdevice":
                continue
            body = ("HTTP/1.1 200 OK\r\n"
                    "CACHE-CONTROL: max-age=1800\r\nEXT:\r\n"
                    "LOCATION: http://127.0.0.1:" + str(port) + "/desc.xml\r\n"
                    "SERVER: soar-test/1.0 UPnP/1.0\r\n"
                    "ST: " + AVT + "\r\n"
                    "USN: uuid:soar-test-renderer::" + AVT + "\r\n\r\n")
            try:
                s.sendto(body.encode(), addr)
            except OSError:
                pass
    except Exception:
        if not up.is_set():
            down.set()

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", port))
srv.listen(4)

if mode == "ssdp":
    ssdp_up = threading.Event()
    ssdp_down = threading.Event()
    threading.Thread(target=ssdp_responder, args=(ssdp_up, ssdp_down),
                     daemon=True).start()
    # Only the fixture may own the group port; if the join fails the
    # wrapper reports the fixture down and the case skips.
    waited = 0.0
    while not ssdp_up.is_set() and not ssdp_down.is_set() and waited < 5.0:
        time.sleep(0.02)
        waited += 0.02
    if not ssdp_up.is_set():
        print("ssdp-down", flush=True)
        sys.exit(1)
print("ready", flush=True)  # harness reads this line over a pipe

t0 = None  # the position clock starts at the first poll
deadline = time.time() + 240
srv.settimeout(max(1.0, deadline - time.time()))
while time.time() < deadline:
    try:
        conn, _ = srv.accept()
    except socket.timeout:
        break
    try:
        conn.settimeout(10)
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = conn.recv(4096)
            if not chunk:
                break
            buf += chunk
        if b"\r\n\r\n" not in buf:
            continue
        head, _, body = buf.partition(b"\r\n\r\n")
        lines = head.decode("iso-8859-1", "replace").splitlines()
        req = lines[0] if lines else ""
        content_length = 0
        soap_action = ""
        for line in lines[1:]:
            low = line.lower()
            if low.startswith("content-length:"):
                content_length = int(line.split(":", 1)[1].strip() or "0")
            elif low.startswith("soapaction:"):
                soap_action = line.split(":", 1)[1].strip()
        while len(body) < content_length:
            chunk = conn.recv(4096)
            if not chunk:
                break
            body += chunk
        text = body.decode("utf-8", "replace")
        path = req.split(" ")[1] if " " in req else "/"
        if path == "/desc.xml":
            conn.sendall(reply(DESC).encode())
            continue
        if path != "/ctl":
            conn.sendall(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0"
                         b"\r\nConnection: close\r\n\r\n")
            continue
        action = soap_action.split("#")[-1].strip('"') if soap_action else "?"
        if action == "SetAVTransportURI":
            m = re.search(r"<CurrentURI>(.*?)</CurrentURI>", text)
            uri = m.group(1) if m else ""
            log("seturi " + uri)
            # A real DMR fetches the source as soon as it accepts it; do
            # the same so the case can pin the rewritten bridge URL as
            # actually serving bytes.
            try:
                r = urllib.request.Request(uri, headers={"Range": "bytes=0-1023"})
                with urllib.request.urlopen(r, timeout=5) as f:
                    n = len(f.read())
                    log("fetch %d %d" % (f.getcode(), n))
            except Exception as e:
                log("fetch-fail %s" % e)
            conn.sendall(reply(action_ok(action)).encode())
        elif action == "GetPositionInfo":
            if t0 is None:
                t0 = time.time()
            rel = min(int(time.time() - t0) + 1, 119)
            if rel < 60:
                pos = "0:00:%02d" % rel
            else:
                pos = "0:%02d:%02d" % (rel // 60, rel % 60)
            payload = ('<?xml version="1.0"?><s:Envelope '
                       'xmlns:s="http://schemas.xmlsoap.org/soap/envelope/">'
                       '<s:Body><u:GetPositionInfoResponse '
                       'xmlns:u="urn:schemas-upnp-org:service:AVTransport:1">'
                       '<Track>1</Track>'
                       '<TrackDuration>0:02:00</TrackDuration>'
                       '<TrackMetaData></TrackMetaData>'
                       '<TrackURI></TrackURI>'
                       '<RelTime>' + pos + '</RelTime>'
                       '<AbsTime>' + pos + '</AbsTime>'
                       '<RelCount>2147483647</RelCount>'
                       '<AbsCount>2147483647</AbsCount>'
                       '</u:GetPositionInfoResponse></s:Body></s:Envelope>')
            conn.sendall(reply(payload).encode())
        else:
            if action == "Play":
                log("play")
                conn.sendall(reply(action_ok(action)).encode())
                if mode == "silent":
                    # Die right after accepting the source: the control
                    # point's polls then fail until the session declares
                    # the receiver dead.
                    conn.close()
                    sys.exit(0)
            elif action == "Stop":
                log("stop")
            conn.sendall(reply(action_ok(action) if action != "?" else "").encode())
    except Exception:
        pass
    finally:
        try:
            conn.close()
        except Exception:
            pass
)PY";

// Forks the dev seeder (tools/seed_torrent) over a payload directory and
// waits until both the .torrent file exists and the seeding port accepts
// connections. The seeder runs forever; the caller kills it. The .torrent
// carries the metadata locally, so the listing case below never needs a
// swarm transfer — only the playback case does.
struct Seeder {
  pid_t pid = -1;
  std::string torrent;
  int port = 0;

  bool start(const std::string& payload_dir, const std::string& out_torrent, int port_) {
    port = port_;
    torrent = out_torrent;
    pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
      ::execlp(SOAR_SEED_TORRENT, SOAR_SEED_TORRENT,
               ("--file=" + payload_dir).c_str(),
               ("--out=" + out_torrent).c_str(),
               ("--port=" + std::to_string(port)).c_str(),
               static_cast<char*>(nullptr));
      _exit(127);
    }
    for (int attempt = 0; attempt < 60; ++attempt) {
      std::error_code ec;
      if (std::filesystem::exists(out_torrent, ec)) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) {
          sockaddr_in addr{};
          addr.sin_family = AF_INET;
          addr.sin_port = htons(static_cast<uint16_t>(port));
          addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
          const bool up = ::connect(fd, reinterpret_cast<sockaddr*>(&addr),
                                    sizeof(addr)) == 0;
          ::close(fd);
          if (up) return true;
        }
      }
      ::usleep(100 * 1000);
    }
    return false;
  }

  void stop() {
    if (pid > 0) {
      ::kill(pid, SIGTERM);
      ::waitpid(pid, nullptr, 0);
      pid = -1;
    }
  }
};

// The fake DLNA renderer: a forked python3 HTTP server (kDlnaRendererScript)
// that publishes a device description, answers the AVTransport SOAP actions,
// and writes everything it saw to a log file the case reads after the CLI
// has exited. `mode` picks the description flavor: "ok" carries the
// AVTransport service the control point needs, "noav" only RenderingControl,
// "ssdp" is "ok" plus an SSDP responder on the group port so --cast=auto
// discovers this device. start() reports a failure when the responder cannot
// take the group port, so a case skips instead of flaking.
struct DlnaRenderer {
  pid_t pid = -1;
  std::string log_path;
  int port = 0;

  bool start(const std::string& log, int port_, const char* mode) {
    log_path = log;
    port = port_;
    int ready_pipe[2];
    if (::pipe(ready_pipe) != 0) return false;
    pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
      ::close(ready_pipe[0]);
      ::dup2(ready_pipe[1], 1);  // script prints "ready" on stdout
      ::close(ready_pipe[1]);
      ::execlp("python3", "python3", "-c", kDlnaRendererScript,
               std::to_string(port).c_str(), log_path.c_str(), mode,
               static_cast<char*>(nullptr));
      _exit(127);
    }
    ::close(ready_pipe[1]);
    std::string ready_line;
    char ch = 0;
    while (::read(ready_pipe[0], &ch, 1) == 1) {
      ready_line += ch;
      if (ch == '\n') break;
    }
    ::close(ready_pipe[0]);
    return ready_line.find("ready") != std::string::npos;
  }

  void stop() {
    if (pid > 0) {
      ::kill(pid, SIGTERM);
      ::waitpid(pid, nullptr, 0);
      pid = -1;
    }
  }

  std::string logged() const {
    std::ifstream in(log_path);
    std::string all, line;
    while (std::getline(in, line)) all += line + "\n";
    return all;
  }
};

// A loopback port nothing listens on right now: bind to port 0, read the
// kernel's pick, close. Another process could claim it afterwards, but the
// window is tiny and a refused connect is what the case needs.
int closedTcpPort() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return 0;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return 0;
  }
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    ::close(fd);
    return 0;
  }
  // Plain ntohs: Darwin defines it as a macro, and ::macro does not parse.
  const int port = ntohs(addr.sin_port);
  ::close(fd);
  return port;
}

}  // namespace
#endif  // !_WIN32

TEST_CASE("headless --torrent-list prints the file table of a seeded multi-file torrent") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  std::string tmpl = "/tmp/soar_cli_tlist_XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  const char* tmp = ::mkdtemp(buf.data());
  REQUIRE(tmp != nullptr);
  const std::string payload = std::string(tmp) + "/payload";
  REQUIRE(std::filesystem::create_directories(payload));
  // 40 files: past the 32-row table bound, so the truncation arm runs.
  for (int i = 0; i < 40; ++i) {
    std::ofstream out(payload + "/f" + std::to_string(i) + ".bin", std::ios::binary);
    out << std::string(64, 'x');
  }
  const int port = 24400 + (::getpid() % 400);
  Seeder seeder;
  if (!seeder.start(payload, std::string(tmp) + "/x.torrent", port)) {
    MESSAGE("seed_torrent did not come up; skipping");
    std::filesystem::remove_all(tmp);
    return;
  }
  const auto run = runCli({"--headless",
                           "--torrent-list",
                           std::string("--torrent-store=") + tmp + "/store",
                           "--torrent-peer=127.0.0.1:" + std::to_string(port),
                           std::string(tmp) + "/x.torrent"},
                          120);
  seeder.stop();
  CHECK(run.exit_code == 0);
  CHECK(run.output.find("torrent: 40 files") != std::string::npos);
  // --torrent-list is the whole-output path: unlike the in-playback stderr
  // table it prints every row (no 32-row bound), in the seeder's sorted
  // order, so the media-less payload's first and last dummies are present.
  CHECK(run.output.find("[0]") != std::string::npos);
  CHECK(run.output.find("[39]") != std::string::npos);
  std::filesystem::remove_all(tmp);
#endif
}

TEST_CASE("headless run plays a seeded multi-file torrent through the bridge") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
#ifndef SOAR_WITH_FFMPEG
  MESSAGE("no FFmpeg backend; skipping the torrent playback test");
  return;
#else
  std::string media;
  if (!envMediaPath("SOAR_TEST_AUDIO_ONLY", media)) {
    MESSAGE("SOAR_TEST_AUDIO_ONLY not set; skipping the torrent playback test");
    return;
  }
  std::string tmpl = "/tmp/soar_cli_tplay_XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  const char* tmp = ::mkdtemp(buf.data());
  REQUIRE(tmp != nullptr);
  const std::string payload = std::string(tmp) + "/payload";
  REQUIRE(std::filesystem::create_directories(payload));
  // The seeder sorts the payload, so names decide the torrent indices: the
  // media file sorts first, so it is index 0 — the default selection the
  // bridge serves (without the seeder's sort, ext4's readdir hash order put
  // a dummy file at 0 and this case played 64 bytes of filler). The rest
  // are dummies that only exist to make the table multi-file.
  REQUIRE(std::filesystem::copy_file(media, payload + "/00_media.mkv"));
  for (int i = 0; i < 39; ++i) {
    std::ofstream out(payload + "/f" + std::to_string(i) + ".bin", std::ios::binary);
    out << std::string(64, 'x');
  }
  const int port = 24800 + (::getpid() % 400);
  Seeder seeder;
  if (!seeder.start(payload, std::string(tmp) + "/x.torrent", port)) {
    MESSAGE("seed_torrent did not come up; skipping");
    std::filesystem::remove_all(tmp);
    return;
  }
  const auto run = runCli({"--headless",
                           std::string("--torrent-store=") + tmp + "/store",
                           "--torrent-peer=127.0.0.1:" + std::to_string(port),
                           std::string(tmp) + "/x.torrent"},
                          240);
  seeder.stop();
  CHECK(run.exit_code == 0);
  // The stderr table names the file count and the index hint, and truncates
  // at 32 rows (the sorted payload's first 32 files).
  CHECK(run.output.find("torrent: 40 files (pick with --torrent-index=N)") !=
        std::string::npos);
  CHECK(run.output.find("  ...") != std::string::npos);
  // The bridge URL opened and played the selected file to its end.
  CHECK(run.output.find("event: state=2") != std::string::npos);
  CHECK(run.output.find("event: media-info") != std::string::npos);
  std::filesystem::remove_all(tmp);
#endif
#endif
}

TEST_CASE("headless --cast drives a fake DLNA renderer through the bridge") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
#ifndef SOAR_WITH_FFMPEG
  MESSAGE("no FFmpeg backend; skipping the cast test");
  return;
#else
  std::string media;
  if (!envMediaPath("SOAR_TEST_AUDIO_ONLY", media)) {
    MESSAGE("SOAR_TEST_AUDIO_ONLY not set; skipping the cast test");
    return;
  }
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("no python3; skipping the cast test");
    return;
  }
  std::string tmpl = "/tmp/soar_cli_cast_XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  const char* tmp = ::mkdtemp(buf.data());
  REQUIRE(tmp != nullptr);
  const std::string payload = std::string(tmp) + "/payload";
  REQUIRE(std::filesystem::create_directories(payload));
  // Same payload shape as the tplay case: the media file, sorted first,
  // is the bridge's default selection.
  REQUIRE(std::filesystem::copy_file(media, payload + "/00_media.mkv"));
  for (int i = 0; i < 39; ++i) {
    std::ofstream out(payload + "/f" + std::to_string(i) + ".bin", std::ios::binary);
    out << std::string(64, 'x');
  }
  const int port = 25000 + (::getpid() % 400);
  const int renderer_port = 25200 + (::getpid() % 400);
  const std::string log_path = std::string(tmp) + "/renderer.log";
  Seeder seeder;
  DlnaRenderer renderer;
  if (!seeder.start(payload, std::string(tmp) + "/x.torrent", port) ||
      !renderer.start(log_path, renderer_port, "ok")) {
    MESSAGE("fixture did not come up; skipping");
    renderer.stop();
    seeder.stop();
    std::filesystem::remove_all(tmp);
    return;
  }
  const auto run = runCli({"--headless",
                           std::string("--torrent-store=") + tmp + "/store",
                           "--torrent-peer=127.0.0.1:" + std::to_string(port),
                           std::string("--cast=http://127.0.0.1:") +
                               std::to_string(renderer_port) + "/desc.xml",
                           std::string(tmp) + "/x.torrent"},
                          240);
  seeder.stop();
  renderer.stop();
  CHECK(run.exit_code == 0);
  // The description's friendlyName names the session on stderr.
  CHECK(run.output.find("cast: Test Renderer <- ") != std::string::npos);
  CHECK(run.output.find("event: media-info") != std::string::npos);

  // The handshake from the renderer's side: SetAVTransportURI, Play, then
  // the session destructor's Stop once the headless tail is done.
  const std::string seen = renderer.logged();
  INFO(seen);
  const std::string uri_marker = "seturi http://127.0.0.1:";
  const size_t uri_at = seen.find(uri_marker);
  REQUIRE(uri_at != std::string::npos);
  const size_t uri_begin = uri_at + uri_marker.size();
  size_t uri_end = uri_begin;
  while (uri_end < seen.size() && seen[uri_end] >= '0' && seen[uri_end] <= '9') {
    ++uri_end;
  }
  const std::string cast_port = seen.substr(uri_begin, uri_end - uri_begin);
  REQUIRE(!cast_port.empty());

  // exposeToLan rebinds the bridge to a fresh port before the old listener
  // retires, so the URL handed over can never reuse the startup line's.
  const std::string bridge_marker = "-> http://127.0.0.1:";
  const size_t bridge_at = run.output.find(bridge_marker);
  REQUIRE(bridge_at != std::string::npos);
  const size_t bridge_begin = bridge_at + bridge_marker.size();
  size_t bridge_end = bridge_begin;
  while (bridge_end < run.output.size() && run.output[bridge_end] >= '0' &&
         run.output[bridge_end] <= '9') {
    ++bridge_end;
  }
  const std::string bridge_port =
      run.output.substr(bridge_begin, bridge_end - bridge_begin);
  CHECK(cast_port != bridge_port);
  // And the rewritten URL really serves bytes: the fake renderer fetched
  // the source the way a real DMR would right after accepting it.
  const bool bridge_served = seen.find("fetch 206 ") != std::string::npos ||
                             seen.find("fetch 200 ") != std::string::npos;
  CHECK(bridge_served);
  CHECK(seen.find("\nplay\n") != std::string::npos);
  CHECK(seen.find("\nstop\n") != std::string::npos);
  std::filesystem::remove_all(tmp);
#endif
#endif
}

// The discovery half of the same handshake: --cast=auto runs M-SEARCH on
// the SSDP group and must end up casting to the device the responder
// describes, not just to a URL handed to it on the command line. The
// responder answers at once, so the fixture wins found[0] over any real
// device that honours the MX delay.
TEST_CASE("headless --cast=auto discovers the mock renderer over ssdp") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
#ifndef SOAR_WITH_FFMPEG
  MESSAGE("no FFmpeg backend; skipping the ssdp discovery cast test");
  return;
#else
  std::string media;
  if (!envMediaPath("SOAR_TEST_AUDIO_ONLY", media)) {
    MESSAGE("SOAR_TEST_AUDIO_ONLY not set; skipping the ssdp discovery cast test");
    return;
  }
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("no python3; skipping the ssdp discovery cast test");
    return;
  }
  std::string tmpl = "/tmp/soar_cli_ssdp_XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  const char* tmp = ::mkdtemp(buf.data());
  REQUIRE(tmp != nullptr);
  const std::string payload = std::string(tmp) + "/payload";
  REQUIRE(std::filesystem::create_directories(payload));
  REQUIRE(std::filesystem::copy_file(media, payload + "/00_media.mkv"));
  for (int i = 0; i < 39; ++i) {
    std::ofstream out(payload + "/f" + std::to_string(i) + ".bin", std::ios::binary);
    out << std::string(64, 'x');
  }
  const int port = 26200 + (::getpid() % 400);
  const int renderer_port = 26400 + (::getpid() % 400);
  const std::string log_path = std::string(tmp) + "/renderer.log";
  Seeder seeder;
  DlnaRenderer renderer;
  if (!seeder.start(payload, std::string(tmp) + "/x.torrent", port) ||
      !renderer.start(log_path, renderer_port, "ssdp")) {
    MESSAGE("fixture did not come up; skipping");
    renderer.stop();
    seeder.stop();
    std::filesystem::remove_all(tmp);
    return;
  }
  // Deliberately no --cast-iface here: the fixture joins the group on the
  // default route ("0.0.0.0", the real LAN shape), so a burst pinned to
  // loopback is exactly the interface mismatch the flag exists to fix. The
  // pinned-burst semantics are the component tests' premise (both ends on
  // loopback there); this side pins the flag's parse surface only.
  const auto run = runCli({"--headless",
                           std::string("--torrent-store=") + tmp + "/store",
                           "--torrent-peer=127.0.0.1:" + std::to_string(port),
                           "--cast=auto",
                           std::string(tmp) + "/x.torrent"},
                          240);
  seeder.stop();
  renderer.stop();
  CHECK(run.exit_code == 0);
  // The session named the device from the discovered description, which is
  // only possible if M-SEARCH reached the responder and the location it
  // advertised resolved back to this fixture.
  CHECK(run.output.find("cast: Test Renderer <- ") != std::string::npos);

  const std::string seen = renderer.logged();
  INFO(seen);
  const std::string uri_marker = "seturi http://127.0.0.1:";
  const size_t uri_at = seen.find(uri_marker);
  REQUIRE(uri_at != std::string::npos);
  const size_t uri_begin = uri_at + uri_marker.size();
  size_t uri_end = uri_begin;
  while (uri_end < seen.size() && seen[uri_end] >= '0' && seen[uri_end] <= '9') {
    ++uri_end;
  }
  const std::string cast_port = seen.substr(uri_begin, uri_end - uri_begin);
  REQUIRE(!cast_port.empty());

  // exposeToLan rebinds the bridge to a fresh port before the old listener
  // retires, so the URL handed over can never reuse the startup line's.
  const std::string bridge_marker = "-> http://127.0.0.1:";
  const size_t bridge_at = run.output.find(bridge_marker);
  REQUIRE(bridge_at != std::string::npos);
  const size_t bridge_begin = bridge_at + bridge_marker.size();
  size_t bridge_end = bridge_begin;
  while (bridge_end < run.output.size() && run.output[bridge_end] >= '0' &&
         run.output[bridge_end] <= '9') {
    ++bridge_end;
  }
  const std::string bridge_port =
      run.output.substr(bridge_begin, bridge_end - bridge_begin);
  CHECK(cast_port != bridge_port);
  // And the rewritten URL really serves bytes, the way a real DMR would
  // have fetched the source right after accepting it.
  const bool bridge_served = seen.find("fetch 206 ") != std::string::npos ||
                             seen.find("fetch 200 ") != std::string::npos;
  CHECK(bridge_served);
  CHECK(seen.find("\nplay\n") != std::string::npos);
  CHECK(seen.find("\nstop\n") != std::string::npos);
  std::filesystem::remove_all(tmp);
#endif
#endif
}

TEST_CASE("headless --cast-transcode hands the renderer the TS endpoint") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
#ifndef SOAR_WITH_FFMPEG
  MESSAGE("no FFmpeg backend; skipping the transcode cast test");
  return;
#else
  std::string media;
  if (!envMediaPath("SOAR_TEST_AUDIO_ONLY", media)) {
    MESSAGE("SOAR_TEST_AUDIO_ONLY not set; skipping the transcode cast test");
    return;
  }
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("no python3; skipping the transcode cast test");
    return;
  }
  std::string tmpl = "/tmp/soar_cli_castts_XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  const char* tmp = ::mkdtemp(buf.data());
  REQUIRE(tmp != nullptr);
  const std::string payload = std::string(tmp) + "/payload";
  REQUIRE(std::filesystem::create_directories(payload));
  REQUIRE(std::filesystem::copy_file(media, payload + "/00_media.mkv"));
  for (int i = 0; i < 39; ++i) {
    std::ofstream out(payload + "/f" + std::to_string(i) + ".bin", std::ios::binary);
    out << std::string(64, 'x');
  }
  const int port = 25400 + (::getpid() % 400);
  const int renderer_port = 25600 + (::getpid() % 400);
  const std::string log_path = std::string(tmp) + "/renderer.log";
  Seeder seeder;
  DlnaRenderer renderer;
  if (!seeder.start(payload, std::string(tmp) + "/x.torrent", port) ||
      !renderer.start(log_path, renderer_port, "ok")) {
    MESSAGE("fixture did not come up; skipping");
    renderer.stop();
    seeder.stop();
    std::filesystem::remove_all(tmp);
    return;
  }
  const auto run = runCli({"--headless",
                           std::string("--torrent-store=") + tmp + "/store",
                           "--torrent-peer=127.0.0.1:" + std::to_string(port),
                           std::string("--cast=http://127.0.0.1:") +
                               std::to_string(renderer_port) + "/desc.xml",
                           "--cast-transcode",
                           std::string(tmp) + "/x.torrent"},
                          240);
  seeder.stop();
  renderer.stop();
  CHECK(run.exit_code == 0);
  CHECK(run.output.find("cast: Test Renderer <- ") != std::string::npos);

  // The URI the renderer was handed is the session's own TS endpoint: a
  // port that is neither the bridge's nor the renderer's, and a fetch the
  // endpoint answers with a bare 200 (a live point has no Range).
  const std::string seen = renderer.logged();
  INFO(seen);
  const std::string uri_marker = "seturi http://127.0.0.1:";
  const size_t uri_at = seen.find(uri_marker);
  REQUIRE(uri_at != std::string::npos);
  const size_t uri_begin = uri_at + uri_marker.size();
  size_t uri_end = uri_begin;
  while (uri_end < seen.size() && seen[uri_end] >= '0' && seen[uri_end] <= '9') {
    ++uri_end;
  }
  const std::string cast_port = seen.substr(uri_begin, uri_end - uri_begin);
  REQUIRE(!cast_port.empty());
  const std::string bridge_marker = "-> http://127.0.0.1:";
  const size_t bridge_at = run.output.find(bridge_marker);
  REQUIRE(bridge_at != std::string::npos);
  const size_t bridge_begin = bridge_at + bridge_marker.size();
  size_t bridge_end = bridge_begin;
  while (bridge_end < run.output.size() && run.output[bridge_end] >= '0' &&
         run.output[bridge_end] <= '9') {
    ++bridge_end;
  }
  const std::string bridge_port =
      run.output.substr(bridge_begin, bridge_end - bridge_begin);
  CHECK(cast_port != bridge_port);
  CHECK(cast_port != std::to_string(renderer_port));
  CHECK(seen.find("fetch 200 ") != std::string::npos);
  CHECK(seen.find("fetch 206 ") == std::string::npos);
  CHECK(seen.find("\nplay\n") != std::string::npos);
  CHECK(seen.find("\nstop\n") != std::string::npos);
  std::filesystem::remove_all(tmp);
#endif
#endif
}

TEST_CASE("headless --cast reports a renderer without AVTransport") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
#ifndef SOAR_WITH_FFMPEG
  MESSAGE("no FFmpeg backend; skipping the cast test");
  return;
#else
  std::string media;
  if (!envMediaPath("SOAR_TEST_AUDIO_ONLY", media)) {
    MESSAGE("SOAR_TEST_AUDIO_ONLY not set; skipping the cast test");
    return;
  }
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("no python3; skipping the cast test");
    return;
  }
  std::string tmpl = "/tmp/soar_cli_castnoav_XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  const char* tmp = ::mkdtemp(buf.data());
  REQUIRE(tmp != nullptr);
  const std::string payload = std::string(tmp) + "/payload";
  REQUIRE(std::filesystem::create_directories(payload));
  REQUIRE(std::filesystem::copy_file(media, payload + "/00_media.mkv"));
  const int port = 25000 + (::getpid() % 400);
  const int renderer_port = 25200 + (::getpid() % 400);
  Seeder seeder;
  DlnaRenderer renderer;
  if (!seeder.start(payload, std::string(tmp) + "/x.torrent", port) ||
      !renderer.start(std::string(tmp) + "/renderer.log", renderer_port, "noav")) {
    MESSAGE("fixture did not come up; skipping");
    renderer.stop();
    seeder.stop();
    std::filesystem::remove_all(tmp);
    return;
  }
  const auto run = runCli({"--headless",
                           std::string("--torrent-store=") + tmp + "/store",
                           "--torrent-peer=127.0.0.1:" + std::to_string(port),
                           std::string("--cast=http://127.0.0.1:") +
                               std::to_string(renderer_port) + "/desc.xml",
                           std::string(tmp) + "/x.torrent"},
                          240);
  seeder.stop();
  renderer.stop();
  CHECK(run.exit_code == 1);
  CHECK(run.output.find("Test Renderer has no AVTransport service") !=
        std::string::npos);
  std::filesystem::remove_all(tmp);
#endif
#endif
}

TEST_CASE("headless --cast fails cleanly when the renderer is unreachable") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
#ifndef SOAR_WITH_FFMPEG
  MESSAGE("no FFmpeg backend; skipping the cast test");
  return;
#else
  std::string media;
  if (!envMediaPath("SOAR_TEST_AUDIO_ONLY", media)) {
    MESSAGE("SOAR_TEST_AUDIO_ONLY not set; skipping the cast test");
    return;
  }
  std::string tmpl = "/tmp/soar_cli_castdead_XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  const char* tmp = ::mkdtemp(buf.data());
  REQUIRE(tmp != nullptr);
  const std::string payload = std::string(tmp) + "/payload";
  REQUIRE(std::filesystem::create_directories(payload));
  REQUIRE(std::filesystem::copy_file(media, payload + "/00_media.mkv"));
  const int port = 25000 + (::getpid() % 400);
  const int dead_port = closedTcpPort();
  if (dead_port == 0) {
    MESSAGE("no closed port available; skipping");
    std::filesystem::remove_all(tmp);
    return;
  }
  Seeder seeder;
  if (!seeder.start(payload, std::string(tmp) + "/x.torrent", port)) {
    MESSAGE("seed_torrent did not come up; skipping");
    seeder.stop();
    std::filesystem::remove_all(tmp);
    return;
  }
  const auto run = runCli({"--headless",
                           std::string("--torrent-store=") + tmp + "/store",
                           "--torrent-peer=127.0.0.1:" + std::to_string(port),
                           std::string("--cast=http://127.0.0.1:") +
                               std::to_string(dead_port) + "/desc.xml",
                           std::string(tmp) + "/x.torrent"},
                          240);
  seeder.stop();
  CHECK(run.exit_code == 1);
  CHECK(run.output.find("cannot reach") != std::string::npos);
  std::filesystem::remove_all(tmp);
#endif
#endif
}

// The cast walkthrough injector. Mode "direct" waits for the cast to go
// live in the renderer log, opens the t overlay, clicks Search again (a
// 3s SSDP burst with no responder on the private display) and Stop
// casting, then quits through the Escape storm the other injectors use.
// Mode "auto" only waits out the pre-armed --cast=auto discovery burst
// (which answers nothing here) and quits; mode "fail" opens the overlay
// immediately and quits after a 404ing description killed the handshake.
// All modes deliver real XTEST input on the private X server, so this
// script needs python3-xlib.
constexpr char kCastWalkthroughScript[] = R"PY(
import sys, time
from Xlib import X, display as xdisplay
from Xlib.ext import xtest

display_name, rlog, mode = sys.argv[1], sys.argv[2], sys.argv[3]

d = xdisplay.Display(display_name)

def find_win():
    def walk(w):
        try:
            name = w.get_wm_name()
        except Exception:
            name = None
        if name and "soar" in str(name).lower():
            return w
        for c in w.query_tree().children:
            f = walk(c)
            if f is not None:
                return f
        return None
    return walk(d.screen().root)

def sync():
    d.sync()

def press(keycode):
    xtest.fake_input(d, X.KeyPress, keycode)
    sync()
    time.sleep(0.05)
    xtest.fake_input(d, X.KeyRelease, keycode)
    sync()

def click(x, y):
    xtest.fake_input(d, X.MotionNotify, x=x, y=y)
    sync()
    time.sleep(0.4)
    xtest.fake_input(d, X.ButtonPress, 1)
    sync()
    # Hold across several rendered frames: a press and release inside one
    # long coverage-build frame is one same-frame pair, and ImGui never
    # sees a click.
    time.sleep(0.4)
    xtest.fake_input(d, X.ButtonRelease, 1)
    sync()
    time.sleep(0.8)

def brightness(x, y):
    # 9x9 mean over the root image (Xvfb 24bpp ZPixmap: 4 bytes per pixel,
    # blue first — the average is channel-order agnostic).
    try:
        img = d.screen().root.get_image(x - 4, y - 4, 9, 9, X.ZPixmap, 0xffffffff)
        data = img.data
        total = 0
        n = 0
        for i in range(0, len(data) - 3, 4):
            total += (data[i] + data[i + 1] + data[i + 2]) // 3
            n += 1
        return total / max(n, 1)
    except Exception:
        return 0.0

def text_rows():
    # The overlay is centred on the soar window's client area at 520x420,
    # so its top-left is the window centre less half of that. Fetch the
    # whole region once: a row carrying glyph strokes has a high horizontal
    # variance, a flat row does not. The discovered renderer row is the
    # overlay's last item, so the last band is it — the "No renderers
    # found yet." line sits in the same place, and the renderer log
    # afterwards decides which one was clicked. The Search again button is
    # the third band from the bottom in every phase (checkbox, row).
    ox = max(0, cx - 260)
    oy = max(0, cy - 210)
    try:
        img = d.screen().root.get_image(ox, oy, 520, 420, X.ZPixmap, 0xffffffff)
    except Exception:
        return [], ox
    hot = []
    for y in range(420):
        row = img.data[y * 520 * 4:(y + 1) * 520 * 4]
        vals = [(row[x * 4] + row[x * 4 + 1] + row[x * 4 + 2]) // 3
                for x in range(20, 500, 3)]
        mean = sum(vals) / len(vals)
        hot.append(sum((v - mean) ** 2 for v in vals) / len(vals) > 25.0)
    rows = []
    y = 0
    while y < 420:
        if not hot[y]:
            y += 1
            continue
        start = y
        while y < 420 and hot[y]:
            y += 1
        if y - start >= 4:  # a text line, not a 1px separator rule
            rows.append((oy + start, oy + y))
    return rows, ox

def escape_until_gone():
    esc = d.keysym_to_keycode(0xFF1B)  # XK_Escape
    deadline = time.monotonic() + 60.0
    while time.monotonic() < deadline:
        win = find_win()
        if win is None:
            return True
        try:
            win.set_input_focus(X.RevertToParent, X.CurrentTime)
        except Exception:
            pass
        sync()
        press(esc)
        time.sleep(0.5)
    return find_win() is None

if mode in ("direct", "autolive"):
    live = False
    for _ in range(240):
        try:
            if "\nplay\n" in open(rlog).read():
                live = True
                break
        except FileNotFoundError:
            pass
        time.sleep(0.5)
    if not live:
        escape_until_gone()
        sys.exit(3)
    time.sleep(1.5)

win = None
for _ in range(60):
    win = find_win()
    if win is not None:
        break
    time.sleep(0.5)
if win is None:
    sys.exit(3)

# get_geometry's x/y are parent-relative, but a top-level window's parent
# is the root, so they are absolute here (translate_coords reads the other
# side of the translation and led every early probe astray).
geo = win.get_geometry()
cx = geo.x + geo.width // 2
cy = geo.y + geo.height // 2

if mode == "direct":
    try:
        win.set_input_focus(X.RevertToParent, X.CurrentTime)
        sync()
        t_key = d.keysym_to_keycode(ord("t"))
        # The Stop button sits one text line below the position readout,
        # and that line appears on the first GetPositionInfo poll (~1s
        # cadence). Until it does the whole block sits one line higher, so
        # wait for the button's pixels before trusting either coordinate.
        stop_x, stop_y = cx - 197, cy - 114
        search_x, search_y = cx - 197, cy - 89
        for _ in range(24):
            if brightness(stop_x, stop_y) > 35.0:
                break
            press(t_key)  # a lost first press; re-pressing re-opens
            time.sleep(1.0)
        if brightness(stop_x, stop_y) > 35.0:
            # Three presses, three guards: the first starts a burst, the
            # second lands while it is still running (one-at-a-time), the
            # third after it finished reaps the dead thread and respawns.
            click(search_x, search_y)
            time.sleep(1.0)
            click(search_x, search_y)
            time.sleep(4.0)  # the 3s discovery window plus its toast
            click(search_x, search_y)
            time.sleep(4.0)
            click(stop_x, stop_y)
            time.sleep(1.5)
    except Exception:
        pass
elif mode == "auto":
    # Give the async torrent time to serve and the pre-armed --cast=auto
    # burst its full 3s window (plus toast margin) before the quit storm,
    # so the discovery path actually runs.
    time.sleep(12.0)
elif mode == "autolive":
    # --cast=auto against a responder that answers: the pre-window wait
    # already let the auto handoff go live, so there is nothing to click.
    pass
elif mode == "discover":
    # Manual discovery: no --cast was pre-armed, so the overlay's own
    # Search again button drives the burst and the discovered row is the
    # way into the cast. No probed coordinate is safe here — the Casting
    # phase's extra lines move the Search button by tens of pixels — so the
    # rows are located from the rendered pixels instead.
    try:
        win.set_input_focus(X.RevertToParent, X.CurrentTime)
        sync()
        press(d.keysym_to_keycode(ord("t")))
    except Exception:
        pass
    time.sleep(2.0)
    rows, ox = text_rows()
    if len(rows) >= 3:
        click(ox + 40, (rows[-3][0] + rows[-3][1]) // 2)  # Search again
    time.sleep(4.5)  # the 3s discovery window plus its margin
    live = False
    for _ in range(3):
        rows, ox = text_rows()
        if not rows:
            break
        click(ox + 40, (rows[-1][0] + rows[-1][1]) // 2)  # the renderer row
        for _ in range(6):
            try:
                if "\nplay\n" in open(rlog).read():
                    live = True
                    break
            except FileNotFoundError:
                pass
            time.sleep(1.0)
        if live:
            break
    if not live:
        escape_until_gone()
        sys.exit(3)
    time.sleep(1.5)
else:
    # "fail"/"silent": open the overlay immediately — the async torrent is
    # usually still preparing, so the not-yet-serving line shows — and
    # keep it open across the failure. "silent" needs the longer wait:
    # the receiver dies after Play and the poll failures take ~10s to
    # declare it dead.
    try:
        win.set_input_focus(X.RevertToParent, X.CurrentTime)
        sync()
        press(d.keysym_to_keycode(ord("t")))
    except Exception:
        pass
    time.sleep(16.0 if mode == "silent" else 10.5)

if mode == "notorrent":
    # No torrent at all: the overlay's no-source explanation text.
    try:
        win.set_input_focus(X.RevertToParent, X.CurrentTime)
        sync()
        press(d.keysym_to_keycode(ord("t")))
    except Exception:
        pass
    time.sleep(2.5)

if mode == "wait":
    # A source that never serves (trackerless magnet): the pre-armed cast
    # request keeps waiting, and the early overlay shows the not-yet-
    # serving line the whole time.
    try:
        win.set_input_focus(X.RevertToParent, X.CurrentTime)
        sync()
        press(d.keysym_to_keycode(ord("t")))
    except Exception:
        pass
    time.sleep(8.0)

if not escape_until_gone():
    sys.exit(4)
sys.exit(0)
)PY";

// The walkthrough injectors drive the window concurrently with the CLI
// child and give up on their own deadline if the window never closes.
// Their exit status classifies a child-side timeout: 0 means the window
// was closed (a nonzero cli exit is then the child's own doing), while
// the escape-until-gone timeout (4) means the injection never landed and
// the child sat in its event loop until the watchdog fired.
#ifndef _WIN32
static int injectorExit(pid_t pid) {
  int status = 0;
  ::waitpid(pid, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
#endif

TEST_CASE("windowed --cast drives the overlay and the renderer end to end") {
#ifdef _WIN32
  MESSAGE("the X11 cast walkthrough is POSIX-only; skipping");
  return;
#else
#ifndef SOAR_CLI_HAS_IMGUI
  MESSAGE("no ImGui; the cast overlay does not exist; skipping");
  return;
#else
#ifndef SOAR_WITH_FFMPEG
  MESSAGE("no FFmpeg backend; skipping the X11 cast walkthrough");
  return;
#else
  std::string media;
  if (!envMediaPath("SOAR_TEST_AUDIO_ONLY", media)) {
    MESSAGE("SOAR_TEST_AUDIO_ONLY not set; skipping the X11 cast walkthrough");
    return;
  }
  if (std::getenv("SOAR_TEST_X11") == nullptr) {
    MESSAGE("SOAR_TEST_X11 not set; skipping the X11 cast walkthrough");
    return;
  }
  if (std::system("command -v Xvfb >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1 && "
                  "python3 -c 'from Xlib.ext import xtest' >/dev/null 2>&1") != 0) {
    MESSAGE("Xvfb or python3-xlib missing; skipping the X11 cast walkthrough");
    return;
  }
  std::string tmpl = "/tmp/soar_cli_castx11_XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  const char* tmp = ::mkdtemp(buf.data());
  REQUIRE(tmp != nullptr);
  const std::string payload = std::string(tmp) + "/payload";
  REQUIRE(std::filesystem::create_directories(payload));
  REQUIRE(std::filesystem::copy_file(media, payload + "/00_media.mkv"));
  const int port = 25000 + (::getpid() % 400);
  const int renderer_port = 25200 + (::getpid() % 400);
  const std::string log_path = std::string(tmp) + "/renderer.log";
  Seeder seeder;
  DlnaRenderer renderer;
  if (!seeder.start(payload, std::string(tmp) + "/x.torrent", port) ||
      !renderer.start(log_path, renderer_port, "ok")) {
    MESSAGE("fixture did not come up; skipping");
    renderer.stop();
    seeder.stop();
    std::filesystem::remove_all(tmp);
    return;
  }

  // Same private-display scheme as the other window tests (this binary
  // runs its cases serially, so the servers never overlap).
  const std::string suffix = std::to_string(70 + (::getpid() % 25));
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
    MESSAGE("Xvfb failed to start; skipping the X11 cast walkthrough");
    ::kill(xvfb, SIGTERM);
    ::waitpid(xvfb, nullptr, 0);
    seeder.stop();
    renderer.stop();
    std::filesystem::remove_all(tmp);
    return;
  }

  // Launch 1: pre-armed direct cast. The window starts the session itself
  // once the async torrent serves, the overlay shows the live position,
  // and the injector's two clicks exercise search + stop before quitting.
  pid_t injector = ::fork();
  REQUIRE(injector >= 0);
  if (injector == 0) {
    ::execlp("python3", "python3", "-c", kCastWalkthroughScript, display.c_str(),
             log_path.c_str(), "direct", static_cast<char*>(nullptr));
    _exit(127);
  }
  const ScopedEnv display_env("DISPLAY", display.c_str());
  const ScopedEnv audio_env("SDL_AUDIODRIVER", "dummy");
  // The walkthrough probes probed every coordinate under the bitmap font;
  // the scaled TTF face reflows the overlay and moves both buttons.
  const ScopedEnv font_env("SOAR_UI_BITMAP_FONT", "1");
  // A windowed child that hangs to the watchdog must leave a stack: the
  // streaming helpers already ride this probe, and the cast walkthrough
  // lost a whole 124 to "no stack, no injector status" (run 38626233481).
  const ScopedEnv hang_dump("SOAR_HANG_DUMP", "1");
  const auto run = runCli({std::string("--torrent-store=") + tmp + "/store",
                           "--torrent-peer=127.0.0.1:" + std::to_string(port),
                           std::string("--cast=http://127.0.0.1:") +
                               std::to_string(renderer_port) + "/desc.xml",
                           std::string(tmp) + "/x.torrent"},
                          300);
  const int injector_rc = injectorExit(injector);
  const std::string seen = renderer.logged();
  std::printf("x11 cast walkthrough: cli exit=%d injector=%d, renderer log:\n%s\n",
              run.exit_code, injector_rc, seen.c_str());

  CHECK(run.exit_code == 0);
  CHECK(run.output.find("event: media-info") != std::string::npos);
  INFO(seen);
  CHECK(seen.find("\nplay\n") != std::string::npos);
  // The overlay's Stop casting button (or the destructor) must have told
  // the renderer to stop — a cast cannot silently outlive the process.
  CHECK(seen.find("\nstop\n") != std::string::npos);

  // Launch 2: --cast=auto with no responder. The burst fails after its
  // window and the session-less t overlay path stays honest; the case
  // pins the clean exit, the coverage numbers carry the rest.
  const std::string empty_log = std::string(tmp) + "/auto.log";
  { std::ofstream touch(empty_log); }
  pid_t injector2 = ::fork();
  REQUIRE(injector2 >= 0);
  if (injector2 == 0) {
    ::execlp("python3", "python3", "-c", kCastWalkthroughScript, display.c_str(),
             empty_log.c_str(), "auto", static_cast<char*>(nullptr));
    _exit(127);
  }
  const auto run2 = runCli({std::string("--torrent-store=") + tmp + "/store2",
                            "--torrent-peer=127.0.0.1:" + std::to_string(port),
                            "--cast=auto",
                            std::string(tmp) + "/x.torrent"},
                           300);
  const int injector2_rc = injectorExit(injector2);
  std::printf("x11 cast auto walkthrough: cli exit=%d injector=%d\n",
              run2.exit_code, injector2_rc);
  CHECK(run2.exit_code == 0);

  // Launch 3: a description URL that answers 404. The handshake fails
  // after the bridge serves, the toast fires, and the overlay (already
  // open) shows the Failed phase text.
  pid_t injector3 = ::fork();
  REQUIRE(injector3 >= 0);
  if (injector3 == 0) {
    ::execlp("python3", "python3", "-c", kCastWalkthroughScript, display.c_str(),
             log_path.c_str(), "fail", static_cast<char*>(nullptr));
    _exit(127);
  }
  const auto run3 = runCli({std::string("--torrent-store=") + tmp + "/store3",
                            "--torrent-peer=127.0.0.1:" + std::to_string(port),
                            std::string("--cast=http://127.0.0.1:") +
                                std::to_string(renderer_port) + "/missing.xml",
                            std::string(tmp) + "/x.torrent"},
                           300);
  const int injector3_rc = injectorExit(injector3);
  std::printf("x11 cast fail walkthrough: cli exit=%d injector=%d\n",
              run3.exit_code, injector3_rc);
  CHECK(run3.exit_code == 0);
  CHECK(run3.output.find("event: media-info") != std::string::npos);

  // Launch 4: a renderer that accepts the source and dies. The poll
  // failures pile up until the session declares the receiver dead, and
  // the t overlay the injector opened early shows the Failed phase.
  const int dead_port = 25400 + (::getpid() % 400);
  DlnaRenderer silent;
  const std::string silent_log = std::string(tmp) + "/silent.log";
  if (!silent.start(silent_log, dead_port, "silent")) {
    MESSAGE("silent renderer did not come up; skipping that launch");
  } else {
    pid_t injector4 = ::fork();
    REQUIRE(injector4 >= 0);
    if (injector4 == 0) {
      ::execlp("python3", "python3", "-c", kCastWalkthroughScript, display.c_str(),
               silent_log.c_str(), "silent", static_cast<char*>(nullptr));
      _exit(127);
    }
    const auto run4 = runCli({std::string("--torrent-store=") + tmp + "/store4",
                              "--torrent-peer=127.0.0.1:" + std::to_string(port),
                              std::string("--cast=http://127.0.0.1:") +
                                  std::to_string(dead_port) + "/desc.xml",
                              std::string(tmp) + "/x.torrent"},
                             300);
    const int injector4_rc = injectorExit(injector4);
    std::printf("x11 cast silent walkthrough: cli exit=%d injector=%d\n",
                run4.exit_code, injector4_rc);
    CHECK(run4.exit_code == 0);
    // The receiver really did accept the source before dying.
    CHECK(silent.logged().find("\nplay\n") != std::string::npos);
  }
  silent.stop();

  // Launch 5: no torrent at all (--gui). The t overlay explains the
  // torrent-only casting contract instead of drawing a status.
  pid_t injector5 = ::fork();
  REQUIRE(injector5 >= 0);
  if (injector5 == 0) {
    ::execlp("python3", "python3", "-c", kCastWalkthroughScript, display.c_str(),
             "-", "notorrent", static_cast<char*>(nullptr));
    _exit(127);
  }
  const auto run5 = runCli({"--gui"}, 120);
  const int injector5_rc = injectorExit(injector5);
  std::printf("x11 cast notorrent walkthrough: cli exit=%d injector=%d\n",
              run5.exit_code, injector5_rc);
  CHECK(run5.exit_code == 0);

  // Launch 6: a trackerless magnet never serves, so the pre-armed cast
  // request keeps waiting through every frame and the early overlay shows
  // the not-yet-serving line. Quitting mid-wait must stay clean.
  pid_t injector6 = ::fork();
  REQUIRE(injector6 >= 0);
  if (injector6 == 0) {
    ::execlp("python3", "python3", "-c", kCastWalkthroughScript, display.c_str(),
             "-", "wait", static_cast<char*>(nullptr));
    _exit(127);
  }
  const auto run6 = runCli({std::string("--torrent-store=") + tmp + "/store6",
                            "--torrent-peer=127.0.0.1:1",
                            std::string("--cast=http://127.0.0.1:") +
                                std::to_string(renderer_port) + "/desc.xml",
                            "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&dn=cast-walkthrough"},
                           300);
  const int injector6_rc = injectorExit(injector6);
  std::printf("x11 cast magnet walkthrough: cli exit=%d injector=%d\n",
              run6.exit_code, injector6_rc);
  CHECK(run6.exit_code == 0);

  // Launches 7 and 8: a responder that answers the SSDP bursts, so the
  // discovery half of the cast actually finds a device. Launch 7 pre-arms
  // --cast=auto and lets the window's own burst hand off to found[0];
  // launch 8 leaves --cast unset, so the overlay's Search again button
  // drives the burst and the discovered row is the way into the cast. The
  // responder answers at once, so the fixture wins found[0] over any real
  // device that honours the MX delay. It comes up only here: an earlier
  // launch must not see it, or launch 2's "no renderer answered" path
  // would start succeeding.
  const int ssdp_port = 25800 + (::getpid() % 400);
  DlnaRenderer ssdp_renderer;
  const std::string ssdp_log = std::string(tmp) + "/ssdp.log";
  if (!ssdp_renderer.start(ssdp_log, ssdp_port, "ssdp")) {
    MESSAGE("the ssdp responder did not come up; skipping those launches");
  } else {
    pid_t injector7 = ::fork();
    REQUIRE(injector7 >= 0);
    if (injector7 == 0) {
      ::execlp("python3", "python3", "-c", kCastWalkthroughScript, display.c_str(),
               ssdp_log.c_str(), "autolive", static_cast<char*>(nullptr));
      _exit(127);
    }
    const auto run7 = runCli({std::string("--torrent-store=") + tmp + "/store7",
                              "--torrent-peer=127.0.0.1:" + std::to_string(port),
                              "--cast=auto",
                              std::string(tmp) + "/x.torrent"},
                             300);
    const int injector7_rc = injectorExit(injector7);
    std::printf("x11 cast auto responder walkthrough: cli exit=%d injector=%d\n",
                run7.exit_code, injector7_rc);
    CHECK(run7.exit_code == 0);
    // The seturi is the end-to-end proof: this device answered the burst,
    // the auto handoff picked it, and the handshake reached it.
    const std::string auto_seen = ssdp_renderer.logged();
    INFO(auto_seen);
    CHECK(auto_seen.find("seturi ") != std::string::npos);
    CHECK(auto_seen.find("\nplay\n") != std::string::npos);
    CHECK(auto_seen.find("\nstop\n") != std::string::npos);

    pid_t injector8 = ::fork();
    REQUIRE(injector8 >= 0);
    if (injector8 == 0) {
      ::execlp("python3", "python3", "-c", kCastWalkthroughScript, display.c_str(),
               ssdp_log.c_str(), "discover", static_cast<char*>(nullptr));
      _exit(127);
    }
    const auto run8 = runCli({std::string("--torrent-store=") + tmp + "/store8",
                              "--torrent-peer=127.0.0.1:" + std::to_string(port),
                              std::string(tmp) + "/x.torrent"},
                             300);
    const int injector8_rc = injectorExit(injector8);
    std::printf("x11 cast discover walkthrough: cli exit=%d injector=%d\n",
                run8.exit_code, injector8_rc);
    CHECK(run8.exit_code == 0);
    // The injector's row click is what put the cast on this device: with
    // no --cast, nothing else could have started the session.
    const std::string discover_seen = ssdp_renderer.logged();
    INFO(discover_seen);
    CHECK(discover_seen.find("seturi ") != std::string::npos);
    CHECK(discover_seen.find("\nplay\n") != std::string::npos);
  }
  ssdp_renderer.stop();

  ::kill(xvfb, SIGTERM);
  ::waitpid(xvfb, nullptr, 0);
  seeder.stop();
  renderer.stop();
  std::filesystem::remove_all(tmp);
#endif
#endif
#endif
}
#endif  // SOAR_SEED_TORRENT

TEST_CASE("--cast without a torrent source is rejected before anything opens") {
  // The torrent-only guard fires before the player opens anything, so no
  // fixture is needed: the source argument never gets opened.
  const auto run = runCli({"--headless", "--cast=http://127.0.0.1:1/desc.xml",
                           "definitely-not-a-torrent.mkv"});
  CHECK(run.exit_code == 2);
  CHECK(run.output.find("--cast needs a torrent source") != std::string::npos);
}

TEST_CASE("--cast-iface parses like the other value flags") {
  // An empty value is the standard exit-2 shape (the same arm --cast= with
  // no value takes), and it is reached during the parse — no fixture, and
  // never through the unknown-option arm.
  const auto run = runCli({"--headless", "--cast-iface="});
  CHECK(run.exit_code == 2);
  CHECK(run.output.find("--cast-iface needs a dotted-quad address") !=
        std::string::npos);
  CHECK(run.output.find("Unknown option") == std::string::npos);
  // A well-formed flag never trips that arm either; with no source the
  // usual usage exit still wins.
  const auto ok = runCli({"--headless", "--cast-iface=127.0.0.1"});
  CHECK(ok.exit_code == 2);
  CHECK(ok.output.find("Unknown option") == std::string::npos);
}

TEST_CASE("--open-dialog parses and stays out of the headless flow") {
  // A window-only flag: in headless it changes nothing about the run, so
  // the null-backend sample still completes (and the flag never trips the
  // unknown-option arm). Help documents both new flags.
  const auto run = runCli({"--headless", "--open-dialog", "--backend=null",
                           "asset://sample"});
  CHECK(run.exit_code == 0);
  CHECK(run.output.find("Unknown option") == std::string::npos);
  const auto help = runCli({"--help"});
  CHECK(help.output.find("--open-dialog") != std::string::npos);
  CHECK(help.output.find("--cast-iface=") != std::string::npos);
}

TEST_CASE("headless run over the null backend succeeds") {
  const auto run = runCli({"--headless", "--backend=null", "asset://sample"});
  CHECK(run.exit_code == 0);
  CHECK(run.output.find("Using null backend") != std::string::npos);
  // The headless flow announces the media and drives seek/pause/stop.
  CHECK(run.output.find("=== Media Info ===") != std::string::npos);
  CHECK(run.output.find("event: media-info") != std::string::npos);
}

TEST_CASE("the default backend resolves to FFmpeg when built in") {
  // No --backend on the command line: the run must pick the real decoder,
  // not the fake-media null backend — window sessions open more sources
  // later (file dialog, drag-and-drop) and those need actual decoding
  // (BUGS.md #3). Same fixture guard as the media-backed cases below.
  std::string media;
  if (!envMediaPath("SOAR_TEST_MEDIA", media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping the default-backend test");
    return;
  }
  const auto run = runCli({"--headless", media});
  CHECK(run.exit_code == 0);
#ifdef SOAR_WITH_FFMPEG
  CHECK(run.output.find("Using FFmpeg backend") != std::string::npos);
#else
  CHECK(run.output.find("Using null backend") != std::string::npos);
#endif
}

TEST_CASE("headless run over an unopenable source fails with 1") {
  // "fail-open" makes the null backend simulate a failed open.
  const auto run = runCli({"--headless", "--backend=null", "asset://fail-open.mp4"});
  CHECK(run.exit_code == 1);
  CHECK(run.output.find("Failed to open source: asset://fail-open.mp4") != std::string::npos);
}

TEST_CASE("a failed open appends its reason to the SOAR_STARTUP_LOG") {
  // Startup failure visibility (BUGS.md "black box flash"): a failure-class
  // early exit must leave a trace in the log a double-clicked user could
  // still find. Relative path on purpose: the child inherits this
  // process's working directory, and a bare filename survives runCli's
  // Windows quoting rules.
  const std::string log = "soar_startup_report_test.log";
  std::remove(log.c_str());
  {
    const ScopedEnv log_env("SOAR_STARTUP_LOG", log.c_str());
    const auto run = runCli({"--headless", "--backend=null", "asset://fail-open.mp4"});
    CHECK(run.exit_code == 1);
  }
  std::ifstream in(log);
  REQUIRE(in.good());
  std::string logged((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
  in.close();
  std::remove(log.c_str());
  // One timestamped entry: the message itself spans two lines (uri line +
  // backend Error line), each newline from a single log call.
  CHECK(logged.find("soar: Failed to open source: asset://fail-open.mp4") !=
        std::string::npos);
  CHECK(logged.find("Error: ") != std::string::npos);
  CHECK(std::count(logged.begin(), logged.end(), '\n') == 2);
}

TEST_CASE("a window-mode startup failure still reaches the log") {
  // The startAsync arm of the torrent start (window mode) is a different
  // early-exit point from the headless one above; it must log the same
  // way. The malformed magnet fails during the synchronous prepare half,
  // before any window or SDL call, so no display is needed.
  const std::string log = "soar_startup_report_test.log";
  std::remove(log.c_str());
  {
    const ScopedEnv log_env("SOAR_STARTUP_LOG", log.c_str());
    const auto run = runCli({"magnet:?dn=broken"});
    CHECK(run.exit_code == 1);
  }
  std::ifstream in(log);
  REQUIRE(in.good());
  std::string logged((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
  in.close();
  std::remove(log.c_str());
  CHECK(logged.find("torrent: cannot parse magnet URI") != std::string::npos);
}

TEST_CASE("usage exits write no startup log") {
  // Usage-class exits (no arguments here) are expected behavior, not
  // failures: they show the dialog at most and must not litter the log.
  const std::string log = "soar_startup_report_test.log";
  std::remove(log.c_str());
  {
    const ScopedEnv log_env("SOAR_STARTUP_LOG", log.c_str());
    const auto run = runCli({});
    CHECK(run.exit_code == 2);
  }
  std::ifstream in(log);
  CHECK(!in.good());
}

TEST_CASE("headless FFmpeg run over a missing file fails with the demuxer error") {
  std::string media;
  if (!envMediaPath("SOAR_TEST_MEDIA", media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping missing-file CLI test");
    return;
  }

  // The same "open failed" contract as the null-backend case above, but
  // over the real backend: the demuxer refuses the file, the backend
  // records the reason, and the CLI has to print it and exit 1 instead of
  // starting a window or a decode thread for a source that never opened.
  const std::string missing = media + ".does-not-exist.mkv";
  const auto run = runCli({"--headless", "--backend=ffmpeg", missing});
  CHECK(run.exit_code == 1);
  CHECK(run.output.find("Failed to open source: " + missing) != std::string::npos);
  CHECK(run.output.find("Error: ") != std::string::npos);
  CHECK(run.output.find("event: error=") != std::string::npos);
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
  CHECK_MESSAGE(run.exit_code == 1, "child output:\n", run.output);
}

TEST_CASE("queue positionals drop empty and option-shaped arguments") {
  // The queue loop behind the first positional keeps only real paths: an
  // empty argument short-circuits before the subscript, and an argument
  // starting with '-' fails the guard — neither may reach the play
  // queue. (The duplicate option is re-parsed harmlessly by the upfront
  // arg loop; it exists to be filtered here.) Like the case above, the
  // run dies at the renderer under the dummy driver — but only after the
  // loop has walked every argument, so the child still reports the exit
  // code this asserts on.
  const ScopedEnv video("SDL_VIDEODRIVER", "dummy");
  const ScopedEnv audio("SDL_AUDIODRIVER", "dummy");
  const auto run = runCli({"--backend=null", "asset://sample", "",
                           "--backend=null"});
  if (run.output.find("SDL_CreateRenderer failed") == std::string::npos) {
    MESSAGE("SDL2 not compiled in; queue loop absent; skipping");
    return;
  }
  // The child's merged output rides the CHECK (run 38786141262: the tsan
  // leg exited 66 here — a detected race — and the report stayed trapped
  // in this string while the log showed only "66 != 1", undiagnosable).
  CHECK_MESSAGE(run.exit_code == 1, "child output:\n", run.output);
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

TEST_CASE("windowed run honors a rebound quit key from the shortcuts config") {
#ifdef _WIN32
  MESSAGE("the X11 window test is POSIX-only; skipping");
  return;
#else
#ifndef SOAR_CLI_HAS_IMGUI
  // Built without the ImGui overlay (CI's no-imgui job): the bare loop
  // consults no shortcut map at all — letters do nothing there by
  // design, and only the hardcoded Escape path quits. Same skip rule as
  // the HUD tour above.
  MESSAGE("app built without the ImGui overlay; skipping the rebind tour");
  return;
#endif
  // Opt-in (the CI coverage job), same gating as the tests above.
  if (std::getenv("SOAR_TEST_X11") == nullptr) {
    MESSAGE("SOAR_TEST_X11 not set; skipping the X11 window test");
    return;
  }
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

  // State scratch: a shortcuts file that moves quit from 'q' to 'x'. The
  // line REPLACES the action's defaults (ui-design.md §3.2), so 'q' is
  // left unbound. XDG_STATE_HOME keeps the window's own state out of the
  // real user store, the same isolation the library/recent cases use.
  const std::string state_home =
      "/tmp/soar_shortcuts_xdg_" + std::to_string(::getpid());
  ::mkdir(state_home.c_str(), 0755);  // EEXIST from a prior run is fine
  const std::string conf_dir = state_home + "/soar";
  ::mkdir(conf_dir.c_str(), 0755);
  {
    std::ofstream out(conf_dir + "/shortcuts.txt", std::ios::trunc);
    out << "quit = x\n";
  }

  // The embedded font pins HUD metrics; audio stays off-pulse.
  const ScopedEnv bitmap_font_env("SOAR_UI_BITMAP_FONT", "1");
  const ScopedEnv display_env("DISPLAY", display.c_str());
  const ScopedEnv audio_env("SDL_AUDIODRIVER", "dummy");
  const ScopedEnv xdg_env("XDG_STATE_HOME", state_home.c_str());

  // The injector storms 'q' (must do nothing now), then presses 'x' and
  // waits for the window to disappear. Its exit code is the rebinding
  // proof; the CLI's exit code only says the session ended.
  pid_t injector = ::fork();
  REQUIRE(injector >= 0);
  if (injector == 0) {
    ::execlp("python3", "python3", "-c", kX11InjectorScript, display.c_str(),
             "soar", "rebind", "0x71", "0x78", static_cast<char*>(nullptr));
    _exit(127);
  }
  const auto run = runCli({"--backend=null", "asset://sample"});
  std::printf("x11 shortcuts rebind test: cli exit=%d, output:\n%s\n",
              run.exit_code, run.output.c_str());
  int injector_rc = -1;
  ::waitpid(injector, &injector_rc, 0);

  if (run.output.find("SDL_CreateRenderer failed") != std::string::npos) {
    MESSAGE("no accelerated renderer under this X server; skipping");
    ::kill(xvfb, SIGTERM);
    ::waitpid(xvfb, nullptr, 0);
    return;
  }
  REQUIRE(WIFEXITED(injector_rc));
  CHECK(WEXITSTATUS(injector_rc) == 0);  // q survived its storm, x quit
  CHECK(run.exit_code == 0);
  ::kill(xvfb, SIGTERM);
  ::waitpid(xvfb, nullptr, 0);
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

// Media-library helpers for the window cases below: the largest position
// event a run produced (a range check, not a substring probe — "321000"
// and "3210" share prefixes and the probe would lie), and the library
// record line of one URI parsed out of the store file.
namespace {
long long maxPositionMs(const std::string& output) {
  long long max_ms = -1;
  std::size_t at = 0;
  while ((at = output.find("event: position=", at)) != std::string::npos) {
    long long v = -1;
    if (std::sscanf(output.c_str() + at, "event: position=%lldms", &v) == 1) {
      if (v > max_ms) max_ms = v;
    }
    at += 16;
  }
  return max_ms;
}

// e <size> <mtime> <pos_ms> <dur_ms> <last_played> <plays> <path>
bool libraryRecord(const std::string& store, const std::string& uri,
                   long long& position_ms, long long& plays) {
  std::ifstream in(store);
  if (!in.good()) return false;
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("e ", 0) != 0) continue;
    if (line.find(uri) == std::string::npos) continue;
    long long size = 0, mtime = 0, pos_ms = 0, dur = 0, played = 0;
    unsigned play_count = 0;
    if (std::sscanf(line.c_str(), "e %lld %lld %lld %lld %lld %u", &size,
                    &mtime, &pos_ms, &dur, &played, &play_count) != 6) {
      return false;
    }
    position_ms = pos_ms;
    plays = play_count;
    return true;
  }
  return false;
}
}  // namespace

TEST_CASE("windowed run resumes a library source; --no-resume declines the jump") {
#ifdef _WIN32
  MESSAGE("the X11 window test is POSIX-only; skipping");
  return;
#else
#ifndef SOAR_CLI_HAS_IMGUI
  // The resume jump and the progress writes live in the HUD (constructor,
  // frame loop, destructor); the bare-SDL build has no HUD at all.
  MESSAGE("app built without the ImGui overlay; skipping the library test");
  return;
#else
  if (std::getenv("SOAR_TEST_X11") == nullptr) {
    MESSAGE("SOAR_TEST_X11 not set; skipping the X11 window test");
    return;
  }
  if (std::system("command -v Xvfb >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1 && "
                  "python3 -c 'from Xlib.ext import xtest' >/dev/null 2>&1") != 0) {
    MESSAGE("Xvfb or python3-xlib missing; skipping the X11 window test");
    return;
  }

  // Same private-display scheme as the other X11 cases; both runs below
  // share this one server, sequentially.
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

  // State scratch. The seeded record points at the null backend's
  // simulated 10-minute media with 5:21 on the clock: past the resume
  // policy's noise floor, and a position no seconds-long session can
  // reach by simply playing.
  const std::string state_home =
      "/tmp/soar_library_xdg_" + std::to_string(::getpid());
  ::mkdir(state_home.c_str(), 0755);  // EEXIST from a prior run is fine
  const std::string seed_dir = state_home + "/soar";
  ::mkdir(seed_dir.c_str(), 0755);
  const std::string store = seed_dir + "/library.txt";
  const char* seed_line = "e 0 0 321000 600000 1700000000 3 asset://sample\n";
  const auto seed_store = [&] {
    std::ofstream out(store, std::ios::trunc);
    out << seed_line;
  };

  // The embedded font pins HUD metrics; audio stays off-pulse; the state
  // dir keeps the run's writes out of the real user store.
  const ScopedEnv bitmap_font_env("SOAR_UI_BITMAP_FONT", "1");
  const ScopedEnv display_env("DISPLAY", display.c_str());
  const ScopedEnv audio_env("SDL_AUDIODRIVER", "dummy");
  const ScopedEnv xdg_env("XDG_STATE_HOME", state_home.c_str());

  // Run 1: the saved position is jumped to and the session re-records it.
  pid_t injector = ::fork();
  REQUIRE(injector >= 0);
  if (injector == 0) {
    ::execlp("python3", "python3", "-c", kX11InjectorScript, display.c_str(),
             "soar", "escape", static_cast<char*>(nullptr));
    _exit(127);
  }
  seed_store();
  const auto run1 = runCli({"--backend=null", "asset://sample"});
  std::printf("x11 library resume test: cli exit=%d, output:\n%s\n",
              run1.exit_code, run1.output.c_str());
  ::waitpid(injector, nullptr, 0);

  if (run1.output.find("SDL_CreateRenderer failed") != std::string::npos) {
    MESSAGE("no accelerated renderer under this X server; skipping");
    ::kill(xvfb, SIGTERM);
    ::waitpid(xvfb, nullptr, 0);
    return;
  }
  REQUIRE(run1.exit_code == 0);
  CHECK(maxPositionMs(run1.output) >= 321000);
  long long pos = 0;
  long long plays = 0;
  REQUIRE(libraryRecord(store, "asset://sample", pos, plays));
  CHECK(pos >= 321000);   // the jump happened and progress kept recording
  CHECK(pos < 340000);    // the session advanced seconds, not minutes
  CHECK(plays == 4);      // seeded 3, this open counted

  // Run 2: --no-resume declines the jump but not the bookkeeping.
  injector = ::fork();
  REQUIRE(injector >= 0);
  if (injector == 0) {
    ::execlp("python3", "python3", "-c", kX11InjectorScript, display.c_str(),
             "soar", "escape", static_cast<char*>(nullptr));
    _exit(127);
  }
  seed_store();
  const auto run2 = runCli({"--backend=null", "--no-resume", "asset://sample"});
  std::printf("x11 library no-resume test: cli exit=%d, output:\n%s\n",
              run2.exit_code, run2.output.c_str());
  ::waitpid(injector, nullptr, 0);

  ::kill(xvfb, SIGTERM);
  ::waitpid(xvfb, nullptr, 0);

  if (run2.output.find("SDL_CreateRenderer failed") != std::string::npos) {
    MESSAGE("no accelerated renderer under this X server; skipping");
    return;
  }
  REQUIRE(run2.exit_code == 0);
  CHECK(maxPositionMs(run2.output) < 60000);
  REQUIRE(libraryRecord(store, "asset://sample", pos, plays));
  CHECK(pos < 60000);     // progress from this session, no 5:21 jump
  CHECK(plays == 4);      // recording continued under --no-resume
#endif
#endif
}

TEST_CASE("headless runs never touch the media library store") {
  // The state-free contract headless tests lean on: no window session, no
  // state writes — the store file must not even come into existence.
  std::error_code fs_ec;
  const std::string state_home =
      (std::filesystem::temp_directory_path() /
       ("soar_library_headless_" + std::to_string(::getpid())))
          .string();
  std::filesystem::create_directories(state_home, fs_ec);  // idempotent
  const std::string store = state_home + "/soar/library.txt";
  {
    const ScopedEnv xdg_env("XDG_STATE_HOME", state_home.c_str());
    const auto run = runCli({"--headless", "--backend=null", "asset://sample"});
    CHECK(run.exit_code == 0);
    CHECK_FALSE(std::filesystem::exists(store, fs_ec));
    // --no-resume parses on the headless path too (accepted, no-op there).
    const auto run2 = runCli(
        {"--headless", "--backend=null", "--no-resume", "asset://sample"});
    CHECK(run2.exit_code == 0);
    CHECK_FALSE(std::filesystem::exists(store, fs_ec));
  }
  std::filesystem::remove_all(state_home, fs_ec);
}

TEST_CASE("windowed run watches a folder into the library through the overlay") {
#ifdef _WIN32
  MESSAGE("the X11 window test is POSIX-only; skipping");
  return;
#else
#ifndef SOAR_CLI_HAS_IMGUI
  // The library overlay, the picker's watch mode and the B key are all
  // HUD-side; the bare-SDL build has none of them.
  MESSAGE("app built without the ImGui overlay; skipping the library overlay test");
  return;
#else
  if (std::getenv("SOAR_TEST_X11") == nullptr) {
    MESSAGE("SOAR_TEST_X11 not set; skipping the library overlay test");
    return;
  }
  if (std::system("command -v Xvfb >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1 && "
                  "python3 -c 'from Xlib.ext import xtest' >/dev/null 2>&1") != 0) {
    MESSAGE("Xvfb or python3-xlib missing; skipping the library overlay test");
    return;
  }

  // 1280x800: the overlay coordinates the injector clicks were probed on
  // exactly this geometry (the picker centres on the display).
  const std::string suffix = std::to_string(70 + (::getpid() % 25));
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
    MESSAGE("Xvfb failed to start; skipping the library overlay test");
    ::kill(xvfb, SIGTERM);
    ::waitpid(xvfb, nullptr, 0);
    return;
  }
  // The socket must answer for OUR Xvfb: a stale Xvfb from an earlier
  // session can own the pid-derived display, so the fork dies on "Server
  // is already active" while the stat succeeds against the leftover's
  // socket. A losing fork lands within moments; require the process to be
  // alive after a short settle, else the overlay would be managed by a
  // foreign X server and the injected clicks would never reach it.
  int xvfb_status = 0;
  bool xvfb_dead = false;
  for (int i = 0; i < 10 && !xvfb_dead; ++i) {
    ::usleep(100 * 1000);
    xvfb_dead = ::waitpid(xvfb, &xvfb_status, WNOHANG) == xvfb;
  }
  if (xvfb_dead) {
    MESSAGE("Xvfb failed to start; skipping the library overlay test");
    ::kill(xvfb, SIGTERM);
    ::waitpid(xvfb, nullptr, 0);
    return;
  }

  // Same staged-HOME scheme as the dir-picker case: ".cache" pins the
  // sorted row order (a dot-directory always sorts first), media_a holds
  // one file at its top and one a level down — the watch-mode picker's
  // recursive count ("2 media files, with subfolders") and the scan's
  // tree walk both read that layout. The files are bare 0-byte names:
  // the scan is extension-based, no media is opened, so the run needs no
  // fixtures and no FFmpeg (the backend stays null behind the poster).
  const std::string home_dir = "/tmp/soar_lib_home_" + std::to_string(::getpid());
  const std::string state_home = "/tmp/soar_lib_xdg_" + std::to_string(::getpid());
  std::error_code fs_ec;
  std::filesystem::remove_all(home_dir, fs_ec);
  std::filesystem::remove_all(state_home, fs_ec);
  REQUIRE(std::filesystem::create_directories(state_home + "/soar", fs_ec));
  REQUIRE(std::filesystem::create_directories(home_dir + "/.cache", fs_ec));
  REQUIRE(std::filesystem::create_directories(home_dir + "/media_a/deep", fs_ec));
  { std::ofstream(home_dir + "/media_a/a1.mkv").put('\n'); }
  { std::ofstream(home_dir + "/media_a/deep/a2.mkv").put('\n'); }
  // a1 also carries an NFO sidecar: the scan picks it up and the overlay
  // shows the scraped title instead of the file name (its hover tooltip
  // gains the plot). Only a1 gets one — a2's row label must stay
  // "a2.mkv" for the drive's x-button coordinate to mean the same thing.
  {
    std::ofstream nfo(home_dir + "/media_a/a1.nfo");
    nfo << "<movie><title>Sample Movie</title><plot>Calibration plot.</plot>"
          "</movie>\n";
  }
  // The Recent bucket orders by the scan's change tokens: pin the files
  // an hour apart so a2 (newer) sorts ahead of a1 and the drive's
  // Recent-row click means the same thing on every run. The token's
  // epoch is the platform's file_clock — only the spacing matters here.
  std::error_code mt_ec;
  const auto ft_now = std::filesystem::file_time_type::clock::now();
  std::filesystem::last_write_time(home_dir + "/media_a/a1.mkv",
                                   ft_now - std::chrono::hours(2), mt_ec);
  REQUIRE_FALSE(mt_ec);
  std::filesystem::last_write_time(home_dir + "/media_a/deep/a2.mkv",
                                   ft_now - std::chrono::hours(1), mt_ec);
  REQUIRE_FALSE(mt_ec);
  const std::string store = state_home + "/soar/library.txt";
  // a1 carries seeded history (5:21 of a 10-minute file, played 3x): the
  // watch's scan merge keeps it, so the overlay renders the resume marker
  // and the tooltip's duration/play-count arms — and the final assertions
  // can prove the history survived the whole round trip untouched.
  {
    std::ofstream seed(store);
    seed << "e 1 0 321000 600000 1700000000 3 " << home_dir << "/media_a/a1.mkv\n";
  }

  pid_t injector = ::fork();
  REQUIRE(injector >= 0);
  if (injector == 0) {
    ::execlp("python3", "python3", "-c", kX11InjectorScript, display.c_str(),
             "soar", "library", store.c_str(), home_dir.c_str(),
             static_cast<char*>(nullptr));
    _exit(127);
  }

  // The picker's start-directory ladder reads HOME; the store and the
  // recent-dirs write under XDG_STATE_HOME — both must stay in the
  // scratch tree.
  const ScopedEnv home_env("HOME", home_dir.c_str());
  const ScopedEnv xdg_env("XDG_STATE_HOME", state_home.c_str());
  const ScopedEnv bitmap_font_env("SOAR_UI_BITMAP_FONT", "1");
  const ScopedEnv display_env("DISPLAY", display.c_str());
  const ScopedEnv audio_env("SDL_AUDIODRIVER", "dummy");
  // No positional, null backend: the run lives on the poster the whole
  // time; every state change comes from the overlay clicks.
  const auto run = runCli({"--gui", "--backend=null"}, 600);

  // injector≠0 ⇒ the click injection never landed (window missing or
  // died early); injector=0 with a cli 124 ⇒ the injection completed
  // while the process refused to exit — the true BUGS.md #4 tail hang.
  // Both land in the log before any assertion: run 38792276031 hung
  // here and the verdict was discarded by the failing REQUIRE.
  int injector_status = 0;
  ::waitpid(injector, &injector_status, 0);
  ::kill(xvfb, SIGTERM);
  ::waitpid(xvfb, nullptr, 0);
  std::printf("x11 library overlay test: cli exit=%d injector=%d, output:\n%s\n",
              run.exit_code,
              WIFEXITED(injector_status) ? WEXITSTATUS(injector_status) : -1,
              run.output.c_str());

  if (run.output.find("SDL_CreateRenderer failed") != std::string::npos) {
    MESSAGE("no accelerated renderer under this X server; skipping");
    std::filesystem::remove_all(home_dir, fs_ec);
    std::filesystem::remove_all(state_home, fs_ec);
    return;
  }
  REQUIRE(run.exit_code == 0);
  // The drive's own verdict: 0 = every step landed. 6 = a bucket-filter
  // no-op click removed a surviving entry (the sidebar leaks), 7 = a2
  // survived its Recent-row "x" (the bucket order, the needle or the
  // right-aligned button drifted), 8 = a1 died under the needle (the
  // search filter leaked). All mean the wiring broke, not just the
  // layout.
  REQUIRE(WIFEXITED(injector_status));
  REQUIRE(WEXITSTATUS(injector_status) == 0);

  // The drive's tail end, asserted from the store it left behind: the
  // injector watched media_a, rescanned, walked the sidebar buckets
  // (Resume and Finished proved leak-free by no-op clicks), focused the
  // search box with "/" and removed a2 through a needle narrowed over
  // the Recent bucket, then unwatched the folder row — each destructive
  // step confirmed against the store before the next click. What remains
  // proves the wiring end to end:
  //   - a1 survives with its seeded history intact (the scan merge keeps
  //     it, removeFolder must not touch non-watched entries, nothing
  //     played so nothing re-recorded);
  //   - a2 is gone (removeEntry via the Recent bucket) and the folder
  //     line is gone (removeFolder).
  std::ifstream lib(store);
  if (!lib.good()) {
    FAIL("library.txt was not written under XDG_STATE_HOME");
  } else {
    const std::string lib_txt((std::istreambuf_iterator<char>(lib)),
                              std::istreambuf_iterator<char>());
    CHECK(lib_txt.find("f ") == std::string::npos);
    CHECK(lib_txt.find("a2.mkv") == std::string::npos);
    long long pos = 0, plays = 0;
    REQUIRE(libraryRecord(store, home_dir + "/media_a/a1.mkv", pos, plays));
    CHECK(pos == 321000);
    CHECK(plays == 3);
    CHECK(lib_txt.find(" 321000 600000 ") != std::string::npos);
  }
  // The watch also lands in the recent-dirs MRU, like an import would
  // (the later unwatch does not unwind it — same as an import).
  std::ifstream recents(state_home + "/soar/recent-dirs.txt");
  if (!recents.good()) {
    FAIL("recent-dirs.txt was not written under XDG_STATE_HOME");
  } else {
    const std::string recents_txt((std::istreambuf_iterator<char>(recents)),
                                  std::istreambuf_iterator<char>());
    CHECK(recents_txt.find("/media_a") != std::string::npos);
  }
  // Nothing opened: the run never leaves the poster, so no media-info
  // markers may appear (the store entries were written by the scan, not
  // by playback).
  CHECK(run.output.find("tracks=") == std::string::npos);

  std::filesystem::remove_all(home_dir, fs_ec);
  std::filesystem::remove_all(state_home, fs_ec);
#endif
#endif
}

TEST_CASE("windowed --gui run with no source opens the empty player") {
#ifdef _WIN32
  MESSAGE("the X11 window test is POSIX-only; skipping");
  return;
#else
  // Opt-in (the CI coverage job), same gating as the window tests above.
  if (std::getenv("SOAR_TEST_X11") == nullptr) {
    MESSAGE("SOAR_TEST_X11 not set; skipping the X11 window test");
    return;
  }
  if (std::system("command -v Xvfb >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1 && "
                  "python3 -c 'from Xlib.ext import xtest' >/dev/null 2>&1") != 0) {
    MESSAGE("Xvfb or python3-xlib missing; skipping the X11 window test");
    return;
  }

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

  // The "empty" injector exercises both open entries (O key, center
  // click) and quits through Escape — the same exit contract as the
  // media-backed window runs.
  pid_t injector = ::fork();
  REQUIRE(injector >= 0);
  if (injector == 0) {
    ::execlp("python3", "python3", "-c", kX11InjectorScript, display.c_str(),
             "soar", "empty", static_cast<char*>(nullptr));
    _exit(127);
  }

  const ScopedEnv display_env("DISPLAY", display.c_str());
  const ScopedEnv audio_env("SDL_AUDIODRIVER", "dummy");
  // No positional: exactly the shape a double-clicked GUI binary (or the
  // Linux .desktop menu item) produces (BUGS.md #3). The run must reach
  // the window and exit cleanly — a usage exit here is the regression
  // this test exists to catch.
  const auto run = runCli({"--gui"});

  std::printf("x11 empty-window test: cli exit=%d, output:\n%s\n", run.exit_code,
              run.output.c_str());

  ::waitpid(injector, nullptr, 0);
  ::kill(xvfb, SIGTERM);
  ::waitpid(xvfb, nullptr, 0);

  if (run.output.find("SDL_CreateRenderer failed") != std::string::npos) {
    // Same environment gap as the Escape test: a skip, not a failure.
    MESSAGE("no accelerated renderer under this X server; skipping");
    return;
  }
  CHECK(run.exit_code == 0);
  CHECK(run.output.find("Usage:") == std::string::npos);
  CHECK(run.output.find("No media source given") == std::string::npos);
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

TEST_CASE("windowed null-backend run hides the OSC on idle and wakes it on motion") {
#ifdef _WIN32
  MESSAGE("the OSC walkthrough is POSIX-only; skipping");
  return;
#else
#ifndef SOAR_CLI_HAS_IMGUI
  // The OSC is an ImGui draw; the bare-SDL build has none of it.
  MESSAGE("app built without the ImGui overlay; skipping the OSC walkthrough");
  return;
#else
  if (std::getenv("SOAR_TEST_X11") == nullptr) {
    MESSAGE("SOAR_TEST_X11 not set; skipping the X11 OSC walkthrough");
    return;
  }
  if (std::system("command -v Xvfb >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1 && "
                  "python3 -c 'from Xlib.ext import xtest' >/dev/null 2>&1") != 0) {
    MESSAGE("Xvfb or python3-xlib missing; skipping the X11 OSC walkthrough");
    return;
  }
  // The injector reads this file while the CLI runs: runCli tees every
  // captured trail byte into it line-flushed (same contract as the
  // playlist walkthrough's event-driven clicks).
  const std::string trail =
      "/tmp/soar_osc_trail_" + std::to_string(::getpid()) + ".log";

  // Same private-display scheme as the drive walkthrough; the cases in
  // this binary run serially, so the display is never contended.
  const std::string suffix = std::to_string(70 + (::getpid() % 25));
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
    MESSAGE("Xvfb failed to start; skipping the X11 OSC walkthrough");
    ::kill(xvfb, SIGTERM);
    ::waitpid(xvfb, nullptr, 0);
    return;
  }

  pid_t injector = ::fork();
  REQUIRE(injector >= 0);
  if (injector == 0) {
    ::execlp("python3", "python3", "-c", kX11InjectorScript, display.c_str(),
             "soar", "osc", trail.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }

  // Keep the run's MRU writes out of the real user state dir, and pin
  // the widget metrics with the embedded bitmap font — both as in the
  // drive test (the coordinates there and in the injector's osc mode
  // mean the same thing only under it).
  const std::string state_home = "/tmp/soar_osc_xdg_" + std::to_string(::getpid());
  ::mkdir(state_home.c_str(), 0755);  // EEXIST from a prior run is fine
  const ScopedEnv xdg_env("XDG_STATE_HOME", state_home.c_str());
  const ScopedEnv bitmap_font_env("SOAR_UI_BITMAP_FONT", "1");
  const ScopedEnv display_env("DISPLAY", display.c_str());
  const ScopedEnv audio_env("SDL_AUDIODRIVER", "dummy");
  // Two queue entries: the injector's prev/next clicks walk a real
  // handover (noseek-live's seekable=false media-info line is the
  // discriminator) besides the two boundary toasts.
  const auto run = runCli({"--backend=null", "asset://sample",
                           "asset://noseek-live"},
                          300, trail);

  std::printf("x11 osc walkthrough: cli exit=%d, output:\n%s\n", run.exit_code,
              run.output.c_str());
  ::waitpid(injector, nullptr, 0);
  ::kill(xvfb, SIGTERM);
  ::waitpid(xvfb, nullptr, 0);
  std::filesystem::remove(trail);
  std::filesystem::remove_all(state_home);

  if (run.output.find("SDL_CreateRenderer failed") != std::string::npos) {
    MESSAGE("no accelerated renderer under this X server; skipping");
    return;
  }
  REQUIRE(run.exit_code == 0);
  // The null backend opened and played (state=2); the injector's space
  // paused it (1) and pinned the bar for the button phase.
  CHECK(run.output.find("event: state=2") != std::string::npos);
  CHECK(run.output.find("event: state=1") != std::string::npos);
  // The auto-hide verdict, edge by edge: hidden after the idle window,
  // shown again on mouse motion alone, hidden once that window lapsed,
  // shown on the key (the injector exits 6 if any edge never came).
  CHECK(run.output.find("event: osc=hidden") != std::string::npos);
  CHECK(run.output.find("event: osc=shown") != std::string::npos);
  const auto count_of = [](const std::string& hay, const std::string& needle) {
    int n = 0;
    for (auto p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size())) {
      ++n;
    }
    return n;
  };
  // The row-2 buttons: prev/next walked a real handover into noseek-live
  // (the only seekable=false media-info) and back (a third open), plus
  // the two boundary toasts, the A-B arm, and the screenshot refusal
  // (the null backend has no frames to save).
  CHECK(count_of(run.output, "event: media-info") >= 3);
  CHECK(run.output.find("seekable=false") != std::string::npos);
  // The paused overlay's round play button resumes: the injector clicks it
  // at the window centre after the pause, so a second state=2 follows the
  // state=1 the space key produced (the first is the null backend opening).
  CHECK(count_of(run.output, "event: state=2") >= 2);
  CHECK(run.output.find("event: toast=Start of playlist") != std::string::npos);
  CHECK(run.output.find("event: toast=End of playlist") != std::string::npos);
  CHECK(run.output.find("event: toast=Loop point A set at") != std::string::npos);
  CHECK(run.output.find("event: toast=Screenshot needs the FFmpeg backend") !=
        std::string::npos);
#endif
#endif
}

TEST_CASE("windowed FFmpeg run renders subtitles and drives the Subtitle Settings page") {
#ifdef _WIN32
  MESSAGE("the X11 window test is POSIX-only; skipping");
  return;
#else
#ifndef SOAR_CLI_HAS_IMGUI
  // The subtitle renderer and the settings page are ImGui widgets; the
  // bare-SDL build has neither, so there is nothing to drive.
  MESSAGE("app built without the ImGui overlay; skipping the subtitle window test");
  return;
#else
  if (std::getenv("SOAR_TEST_X11") == nullptr) {
    MESSAGE("SOAR_TEST_X11 not set; skipping the subtitle window test");
    return;
  }
#ifndef SOAR_WITH_FFMPEG
  MESSAGE("no FFmpeg backend; skipping the subtitle window test");
  return;
#else
  std::string media;
  if (!envMediaPath("SOAR_TEST_SUBS_MEDIA", media)) {
    MESSAGE("SOAR_TEST_SUBS_MEDIA not set; skipping the subtitle window test");
    return;
  }
  if (std::system("command -v Xvfb >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1 && "
                  "python3 -c 'from Xlib.ext import xtest' >/dev/null 2>&1") != 0) {
    MESSAGE("Xvfb or python3-xlib missing; skipping the subtitle window test");
    return;
  }

  // Same private-display scheme as the other X11 cases; cases run serially.
  const std::string suffix = std::to_string(70 + (::getpid() % 25));
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
    MESSAGE("Xvfb failed to start; skipping the subtitle window test");
    ::kill(xvfb, SIGTERM);
    ::waitpid(xvfb, nullptr, 0);
    return;
  }

  pid_t injector = ::fork();
  REQUIRE(injector >= 0);
  if (injector == 0) {
    ::execlp("python3", "python3", "-c", kX11InjectorScript, display.c_str(),
             "soar", "subsdrive", static_cast<char*>(nullptr));
    _exit(127);
  }

  // Keep the run's MRU entries out of the real user state dir.
  const std::string state_home = "/tmp/soar_recent_xdg_" + std::to_string(::getpid()) + "s";
  ::mkdir(state_home.c_str(), 0755);  // EEXIST from a prior run is fine
  const ScopedEnv xdg_env("XDG_STATE_HOME", state_home.c_str());
  // The embedded bitmap font pins widget metrics so the injector's
  // coordinates mean the same thing on every machine.
  const ScopedEnv bitmap_font_env("SOAR_UI_BITMAP_FONT", "1");
  const ScopedEnv display_env("DISPLAY", display.c_str());
  const ScopedEnv audio_env("SDL_AUDIODRIVER", "dummy");
  // The injector's S press makes the child write screenshot.png into its
  // working directory (popen inherits ours); drop any leftover first so
  // a stale file from an earlier run cannot satisfy the check below.
  char cwd_buf[4096];
  REQUIRE(::getcwd(cwd_buf, sizeof(cwd_buf)) != nullptr);
  const std::string shot_path = std::string(cwd_buf) + "/screenshot.png";
  ::unlink(shot_path.c_str());
  // Real FFmpeg backend: the fixture carries an SRT cue track, so the
  // decode thread delivers subtitle frames while the injector drives.
  // Child budget 1500s: the suite's longest scripted session. Under
  // --coverage -O0 the app lags seconds behind the injector, and before
  // the quit loop dismissed stale combo popups (see its comment) a lag
  // pair left one open, swallowed every key including the q storm, and
  // hung the child until the cap killed it — CI runs 36298838808 /
  // 36299389988 / 36301040326 died exactly there. With the dismissal in
  // place a healthy instrumented run takes ~1-2 min; the generous cap
  // only bounds a genuinely wedged child as a normal failing assertion
  // instead of a context-free suite timeout. Every other case fits the
  // default.
  const auto run = runCli({"--backend=ffmpeg", media}, 1500);

  std::printf("x11 subtitle window test: cli exit=%d, output:\n%s\n", run.exit_code,
              run.output.c_str());

  ::waitpid(injector, nullptr, 0);
  ::kill(xvfb, SIGTERM);
  ::waitpid(xvfb, nullptr, 0);

  if (run.output.find("SDL_CreateRenderer failed") != std::string::npos) {
    MESSAGE("no accelerated renderer under this X server; skipping");
    return;
  }
  REQUIRE(run.exit_code == 0);
  // The backend opened and played (state=2); the injector's first gesture
  // is space, and the paused state (1) proves it landed while playback
  // was live — i.e. inside the first cue's display window.
  CHECK(run.output.find("event: state=2") != std::string::npos);
  CHECK(run.output.find("event: state=1") != std::string::npos);
  // Every pick in the tour re-publishes MediaInfo, so the selection
  // trail is visible in the event stream: the settings page and the OSC
  // combo both enable the stream, C disables it again and re-enables it,
  // and the very last selection has to be the enabled one — the C
  // presses are the tail of the tour, so a stream left off would mean
  // the last C never landed.
  const std::size_t sub_on = run.output.find("selected(video=0,audio=1,sub=2)");
  CHECK(sub_on != std::string::npos);
  const std::size_t sub_off = run.output.find("selected(video=0,audio=1,sub=-1)", sub_on);
  CHECK(sub_off != std::string::npos);
  CHECK(run.output.rfind("selected(video=0,audio=1,sub=2)") > sub_off);
  // S (pressed while paused on a live frame) saved a PNG: the backend's
  // saveScreenshot ran through the encoder path and the HUD acknowledged
  // it. Verify the magic bytes, then clean up the working directory.
  {
    std::ifstream shot(shot_path, std::ios::binary);
    REQUIRE(shot.good());
    std::string magic(8, '\0');
    shot.read(magic.data(), 8);
    CHECK(magic == std::string("\x89PNG\r\n\x1a\n", 8));
  }
  ::unlink(shot_path.c_str());
#endif
#endif
#endif
}

TEST_CASE("windowed FFmpeg run loads a sidecar pick from the subtitle menu") {
#ifdef _WIN32
  MESSAGE("the X11 window test is POSIX-only; skipping");
  return;
#else
#ifndef SOAR_CLI_HAS_IMGUI
  // The subtitle menu and its sidecar section are ImGui widgets; the
  // bare-SDL build has neither.
  MESSAGE("app built without the ImGui overlay; skipping the sidecar window test");
  return;
#else
  if (std::getenv("SOAR_TEST_X11") == nullptr) {
    MESSAGE("SOAR_TEST_X11 not set; skipping the sidecar window test");
    return;
  }
#ifndef SOAR_WITH_FFMPEG
  MESSAGE("no FFmpeg backend; skipping the sidecar window test");
  return;
#else
  std::string media;
  if (!envMediaPath("SOAR_TEST_SUBS_MEDIA", media)) {
    MESSAGE("SOAR_TEST_SUBS_MEDIA not set; skipping the sidecar window test");
    return;
  }
  if (std::system("command -v Xvfb >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1 && "
                  "python3 -c 'from Xlib.ext import xtest' >/dev/null 2>&1") != 0) {
    MESSAGE("Xvfb or python3-xlib missing; skipping the sidecar window test");
    return;
  }

  // The sidecar scan reads the media's directory, so the case plays a copy
  // of the fixture from its own scratch dir and stages siblings there. Pass
  // 1 sees exactly one candidate (the good sidecar); the cue-less sibling
  // is staged by the injector only when the failure pass needs it, keeping
  // every popup at one candidate row and every coordinate fixed.
  const std::string scratch = "/tmp/soar_sidecar_x11_" + std::to_string(::getpid());
  std::error_code ec;
  std::filesystem::remove_all(scratch, ec);
  REQUIRE(std::filesystem::create_directories(scratch, ec));
  REQUIRE_FALSE(ec);
  const std::string played = scratch + "/feature_film.mkv";
  std::filesystem::copy_file(media, played,
                             std::filesystem::copy_options::overwrite_existing, ec);
  REQUIRE_FALSE(ec);
  {
    std::ofstream srt(scratch + "/feature_film.en.srt", std::ios::binary);
    REQUIRE(srt.good());
    srt << "1\n00:00:00,500 --> 00:00:02,500\nHello from sidecar\n\n"
           "2\n00:00:03,000 --> 00:00:05,000\nSecond cue\n";
  }
  const std::string broken_sidecar = scratch + "/feature_film.bad.srt";

  // Same private-display scheme as the other X11 cases; cases run serially.
  const std::string suffix = std::to_string(70 + (::getpid() % 25));
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
    MESSAGE("Xvfb failed to start; skipping the sidecar window test");
    ::kill(xvfb, SIGTERM);
    ::waitpid(xvfb, nullptr, 0);
    return;
  }

  pid_t injector = ::fork();
  REQUIRE(injector >= 0);
  if (injector == 0) {
    ::execlp("python3", "python3", "-c", kX11InjectorScript, display.c_str(),
             "soar", "sidecar", broken_sidecar.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }

  const std::string state_home = "/tmp/soar_sidecar_xdg_" + std::to_string(::getpid());
  ::mkdir(state_home.c_str(), 0755);  // EEXIST from a prior run is fine
  const ScopedEnv xdg_env("XDG_STATE_HOME", state_home.c_str());
  // The embedded bitmap font pins widget metrics so the injector's
  // coordinates mean the same thing on every machine.
  const ScopedEnv bitmap_font_env("SOAR_UI_BITMAP_FONT", "1");
  const ScopedEnv display_env("DISPLAY", display.c_str());
  const ScopedEnv audio_env("SDL_AUDIODRIVER", "dummy");
  const auto run = runCli({"--backend=ffmpeg", played});

  std::printf("x11 sidecar window test: cli exit=%d, output:\n%s\n", run.exit_code,
              run.output.c_str());

  ::waitpid(injector, nullptr, 0);
  ::kill(xvfb, SIGTERM);
  ::waitpid(xvfb, nullptr, 0);

  if (run.output.find("SDL_CreateRenderer failed") != std::string::npos) {
    MESSAGE("no accelerated renderer under this X server; skipping");
    std::filesystem::remove_all(scratch, ec);
    return;
  }
  REQUIRE(run.exit_code == 0);
  CHECK(run.output.find("event: state=2") != std::string::npos);
  CHECK(run.output.find("event: state=1") != std::string::npos);
  // The pick loaded the sidecar as a real track: an external id sits past
  // the container's stream range. The subs fixture exposes 3 tracks but
  // carries a font-attachment stream too (nb_streams = 4), so the first
  // sidecar lands on id 4 — the selection trail proves the menu ->
  // loadExternalSubtitle -> selectTrack wiring end to end.
  const std::size_t sidecar_on = run.output.find("selected(video=0,audio=1,sub=4)");
  CHECK(sidecar_on != std::string::npos);
  // The initial media-info legitimately has the subtitle stream deselected;
  // nothing after the pick may have turned subtitles back off — a stray Off
  // pick from a mis-aimed click would show up here.
  if (sidecar_on != std::string::npos) {
    CHECK(run.output.find("selected(video=0,audio=1,sub=-1)", sidecar_on) ==
          std::string::npos);
  }
  std::filesystem::remove_all(scratch, ec);
#endif
#endif
#endif
}

TEST_CASE("windowed FFmpeg run downloads and loads a remote subtitle pick") {
#ifdef _WIN32
  MESSAGE("the X11 window test is POSIX-only; skipping");
  return;
#else
#ifndef SOAR_CLI_HAS_IMGUI
  // The subtitle menu's [download] section is an ImGui widget; the
  // bare-SDL build has neither the menu nor the HTTP client behind it.
  MESSAGE("app built without the ImGui overlay; skipping the download window test");
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
  std::string media;
  if (!envMediaPath("SOAR_TEST_SUBS_MEDIA", media)) {
    MESSAGE("SOAR_TEST_SUBS_MEDIA not set; skipping the download window test");
    return;
  }
  if (std::system("command -v Xvfb >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1 && "
                  "python3 -c 'from Xlib.ext import xtest' >/dev/null 2>&1") != 0) {
    MESSAGE("Xvfb or python3-xlib missing; skipping the download window test");
    return;
  }

  // The external provider scans the media's directory, so the case plays a
  // copy of the fixture from its own scratch dir with no sidecar siblings:
  // the only remote offer is the catalog server's row, staged in dl/ and
  // listed in catalog.tsv. The server reads the catalog per request, so it
  // can start before the file is written. The key is set on both ends, so
  // the env -> X-API-Key wiring is proven, not just tolerated.
  const std::string scratch = "/tmp/soar_subsdl_x11_" + std::to_string(::getpid());
  std::error_code ec;
  std::filesystem::remove_all(scratch, ec);
  REQUIRE(std::filesystem::create_directories(scratch + "/dl", ec));
  REQUIRE_FALSE(ec);
  const std::string played = scratch + "/feature_film.mkv";
  std::filesystem::copy_file(media, played,
                             std::filesystem::copy_options::overwrite_existing, ec);
  REQUIRE_FALSE(ec);
  {
    std::ofstream srt(scratch + "/dl/movie.en.srt", std::ios::binary);
    REQUIRE(srt.good());
    srt << "1\n00:00:00,500 --> 00:00:02,500\nHello from the catalog\n\n"
           "2\n00:00:03,000 --> 00:00:05,000\nSecond cue\n";
  }
  const auto srv = test_servers::startSubtitleServer(scratch, "catalog", "subsdl-key");
  REQUIRE(srv.pid > 0);
  {
    std::ofstream catalog(scratch + "/catalog.tsv", std::ios::binary);
    REQUIRE(catalog.good());
    catalog << "# size hash name\n"
            << srv.base_url << "/dl/movie.en.srt\ten\tCatalog pickup\tsrt\n";
  }

  // Same private-display scheme as the other X11 cases; cases run serially.
  const std::string suffix = std::to_string(70 + (::getpid() % 25));
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
    return;
  }

  pid_t injector = ::fork();
  REQUIRE(injector >= 0);
  if (injector == 0) {
    ::execlp("python3", "python3", "-c", kX11InjectorScript, display.c_str(),
             "soar", "subsdl", static_cast<char*>(nullptr));
    _exit(127);
  }

  const std::string state_home = "/tmp/soar_subsdl_xdg_" + std::to_string(::getpid());
  ::mkdir(state_home.c_str(), 0755);  // EEXIST from a prior run is fine
  const ScopedEnv xdg_env("XDG_STATE_HOME", state_home.c_str());
  // The embedded bitmap font pins widget metrics so the injector's
  // coordinates mean the same thing on every machine.
  const ScopedEnv bitmap_font_env("SOAR_UI_BITMAP_FONT", "1");
  const ScopedEnv display_env("DISPLAY", display.c_str());
  const ScopedEnv audio_env("SDL_AUDIODRIVER", "dummy");
  const ScopedEnv endpoint_env("SOAR_SUBTITLE_ENDPOINT", (srv.base_url + "/search").c_str());
  const ScopedEnv key_env("SOAR_SUBTITLE_API_KEY", "subsdl-key");
  const auto run = runCli({"--backend=ffmpeg", played});

  std::printf("x11 download window test: cli exit=%d, output:\n%s\n", run.exit_code,
              run.output.c_str());

  ::waitpid(injector, nullptr, 0);
  ::kill(xvfb, SIGTERM);
  ::waitpid(xvfb, nullptr, 0);

  if (run.output.find("SDL_CreateRenderer failed") != std::string::npos) {
    MESSAGE("no accelerated renderer under this X server; skipping");
    std::filesystem::remove_all(scratch, ec);
    return;
  }
  REQUIRE(run.exit_code == 0);
  CHECK(run.output.find("event: state=2") != std::string::npos);
  CHECK(run.output.find("event: state=1") != std::string::npos);
  // The pick fetched the catalog's srt, stored it under the state home and
  // loaded it as a real track: an external id sits past the container's
  // stream range. The subs fixture exposes 3 tracks but carries a
  // font-attachment stream too (nb_streams = 4), so the download lands on
  // id 4 — the selection trail proves menu -> fetch -> store ->
  // loadExternalSubtitle -> selectTrack end to end over loopback HTTP.
  const std::size_t download_on = run.output.find("selected(video=0,audio=1,sub=4)");
  CHECK(download_on != std::string::npos);
  // Nothing after the pick may have turned subtitles back off — a stray Off
  // pick from a mis-aimed click would show up here.
  if (download_on != std::string::npos) {
    CHECK(run.output.find("selected(video=0,audio=1,sub=-1)", download_on) ==
          std::string::npos);
  }
  std::filesystem::remove_all(scratch, ec);
#endif
#endif
#endif
}

TEST_CASE("windowed FFmpeg run translates a loaded subtitle track") {
#ifdef _WIN32
  MESSAGE("the X11 window test is POSIX-only; skipping");
  return;
#else
#ifndef SOAR_CLI_HAS_IMGUI
  // The [translate] row is an ImGui widget; the bare-SDL build has neither
  // the menu nor the translator behind it.
  MESSAGE("app built without the ImGui overlay; skipping the translate window test");
  return;
#else
  if (std::getenv("SOAR_TEST_X11") == nullptr) {
    MESSAGE("SOAR_TEST_X11 not set; skipping the translate window test");
    return;
  }
#ifndef SOAR_WITH_FFMPEG
  MESSAGE("no FFmpeg backend; skipping the translate window test");
  return;
#else
  std::string media;
  if (!envMediaPath("SOAR_TEST_SUBS_MEDIA", media)) {
    MESSAGE("SOAR_TEST_SUBS_MEDIA not set; skipping the translate window test");
    return;
  }
  if (std::system("command -v Xvfb >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1 && "
                  "python3 -c 'from Xlib.ext import xtest' >/dev/null 2>&1") != 0) {
    MESSAGE("Xvfb or python3-xlib missing; skipping the translate window test");
    return;
  }

  // Two fixture servers: the catalog hands out the source srt (pick 1
  // downloads and loads it), and the chat server answers the translator's
  // numbered-line batches (pick 2). Both keys are set on both ends, so the
  // env -> header wiring is proven for X-API-Key and Bearer alike. The chat
  // server's requests.log records every POST, which lets the case assert
  // the wire shape — system prompt, model, numbered batch — end to end.
  const std::string scratch = "/tmp/soar_substl_x11_" + std::to_string(::getpid());
  std::error_code ec;
  std::filesystem::remove_all(scratch, ec);
  REQUIRE(std::filesystem::create_directories(scratch + "/dl", ec));
  REQUIRE_FALSE(ec);
  const std::string played = scratch + "/feature_film.mkv";
  std::filesystem::copy_file(media, played,
                             std::filesystem::copy_options::overwrite_existing, ec);
  REQUIRE_FALSE(ec);
  {
    std::ofstream srt(scratch + "/dl/movie.en.srt", std::ios::binary);
    REQUIRE(srt.good());
    srt << "1\n00:00:00,500 --> 00:00:02,500\nHello from the catalog\n\n"
           "2\n00:00:03,000 --> 00:00:05,000\nSecond cue\n";
  }
  const auto srv = test_servers::startSubtitleServer(scratch, "catalog", "substl-key");
  REQUIRE(srv.pid > 0);
  {
    std::ofstream catalog(scratch + "/catalog.tsv", std::ios::binary);
    REQUIRE(catalog.good());
    catalog << "# size hash name\n"
            << srv.base_url << "/dl/movie.en.srt\ten\tCatalog pickup\tsrt\n";
  }
  const auto chat = test_servers::startChatServer(scratch, "ok", "substl-chat-key");
  REQUIRE(chat.pid > 0);

  // Same private-display scheme as the other X11 cases; cases run serially.
  const std::string suffix = std::to_string(70 + (::getpid() % 25));
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
    MESSAGE("Xvfb failed to start; skipping the translate window test");
    ::kill(xvfb, SIGTERM);
    ::waitpid(xvfb, nullptr, 0);
    return;
  }

  pid_t injector = ::fork();
  REQUIRE(injector >= 0);
  if (injector == 0) {
    ::execlp("python3", "python3", "-c", kX11InjectorScript, display.c_str(),
             "soar", "substl", static_cast<char*>(nullptr));
    _exit(127);
  }

  const std::string state_home = "/tmp/soar_substl_xdg_" + std::to_string(::getpid());
  ::mkdir(state_home.c_str(), 0755);  // EEXIST from a prior run is fine
  const ScopedEnv xdg_env("XDG_STATE_HOME", state_home.c_str());
  // The embedded bitmap font pins widget metrics so the injector's
  // coordinates mean the same thing on every machine.
  const ScopedEnv bitmap_font_env("SOAR_UI_BITMAP_FONT", "1");
  const ScopedEnv display_env("DISPLAY", display.c_str());
  const ScopedEnv audio_env("SDL_AUDIODRIVER", "dummy");
  const ScopedEnv endpoint_env("SOAR_SUBTITLE_ENDPOINT", (srv.base_url + "/search").c_str());
  const ScopedEnv key_env("SOAR_SUBTITLE_API_KEY", "substl-key");
  const ScopedEnv tr_endpoint_env("SOAR_TRANSLATE_ENDPOINT", chat.base_url.c_str());
  const ScopedEnv tr_key_env("SOAR_TRANSLATE_API_KEY", "substl-chat-key");
  const ScopedEnv tr_model_env("SOAR_TRANSLATE_MODEL", "substl-model");
  const ScopedEnv tr_lang_env("SOAR_TRANSLATE_LANGUAGE", "Klingon");
  const auto run = runCli({"--backend=ffmpeg", played});

  std::printf("x11 translate window test: cli exit=%d, output:\n%s\n", run.exit_code,
              run.output.c_str());

  ::waitpid(injector, nullptr, 0);
  ::kill(xvfb, SIGTERM);
  ::waitpid(xvfb, nullptr, 0);

  if (run.output.find("SDL_CreateRenderer failed") != std::string::npos) {
    MESSAGE("no accelerated renderer under this X server; skipping");
    std::filesystem::remove_all(scratch, ec);
    return;
  }
  REQUIRE(run.exit_code == 0);
  CHECK(run.output.find("event: state=2") != std::string::npos);
  CHECK(run.output.find("event: state=1") != std::string::npos);
  // Pick 1 loads the download on id 4 (the subs fixture exposes 3 tracks
  // but carries a font-attachment stream too, nb_streams = 4); pick 2
  // loads the translation on id 5. The ordered trail proves menu ->
  // download -> load -> [translate] -> batch chat -> store -> load
  // end to end over loopback HTTP.
  const std::size_t download_on = run.output.find("selected(video=0,audio=1,sub=4)");
  REQUIRE(download_on != std::string::npos);
  const std::size_t translated_on =
      run.output.find("selected(video=0,audio=1,sub=5)", download_on);
  CHECK(translated_on != std::string::npos);
  // Nothing after the pick may have turned subtitles back off — a stray
  // Off pick from a mis-aimed click would show up here.
  if (translated_on != std::string::npos) {
    CHECK(run.output.find("selected(video=0,audio=1,sub=-1)", translated_on) ==
          std::string::npos);
  }
  // The chat server saw exactly the documented wire shape: the env-injected
  // model and Bearer key, the target language from SOAR_TRANSLATE_LANGUAGE
  // in the system prompt, and a numbered batch with one line per cue. (The
  // fixture's requests.log is the test's own recording of the traffic; the
  // product never logs the key itself.)
  {
    std::ifstream log(scratch + "/requests.log");
    REQUIRE(log.good());
    std::string line;
    bool found = false;
    while (std::getline(log, line)) {
      if (line.find("substl-model") == std::string::npos) {
        continue;
      }
      found = true;
      CHECK(line.find("Bearer substl-chat-key") != std::string::npos);
      CHECK(line.find("into Klingon") != std::string::npos);
      CHECK(line.find("1. Hello from the catalog") != std::string::npos);
      CHECK(line.find("2. Second cue") != std::string::npos);
    }
    CHECK(found);
  }
  std::filesystem::remove_all(scratch, ec);
#endif
#endif
#endif
}

TEST_CASE("windowed FFmpeg run rejects an embedded subtitle translate over a failing chat endpoint") {
#ifdef _WIN32
  MESSAGE("the X11 window test is POSIX-only; skipping");
  return;
#else
#ifndef SOAR_CLI_HAS_IMGUI
  MESSAGE("app built without the ImGui overlay; skipping the embedded-translate window test");
  return;
#else
  if (std::getenv("SOAR_TEST_X11") == nullptr) {
    MESSAGE("SOAR_TEST_X11 not set; skipping the embedded-translate window test");
    return;
  }
#ifndef SOAR_WITH_FFMPEG
  MESSAGE("no FFmpeg backend; skipping the embedded-translate window test");
  return;
#else
  std::string media;
  if (!envMediaPath("SOAR_TEST_SUBS_MEDIA", media)) {
    MESSAGE("SOAR_TEST_SUBS_MEDIA not set; skipping the embedded-translate window test");
    return;
  }
  std::string audio_only;
  if (!envMediaPath("SOAR_TEST_AUDIO_ONLY", audio_only)) {
    MESSAGE("SOAR_TEST_AUDIO_ONLY not set; skipping the embedded-translate window test");
    return;
  }
  if (std::system("command -v Xvfb >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1 && "
                  "python3 -c 'from Xlib.ext import xtest' >/dev/null 2>&1") != 0) {
    MESSAGE("Xvfb or python3-xlib missing; skipping the embedded-translate window test");
    return;
  }

  // The embedded-track [translate] pick over a chat fixture that answers
  // every POST with 500: the pick demuxes the container's subrip stream
  // out (the POST the fixture logs proves the export ran), the translate
  // fails and the run degrades to a toast. The injector then overwrites
  // the played copy with the audio-only fixture — the next pick reopens
  // the path and dies earlier, at the export's reopen probe — so both
  // failure arms run and neither may load a track.
  const std::string scratch = "/tmp/soar_subsembed_x11_" + std::to_string(::getpid());
  std::error_code ec;
  std::filesystem::remove_all(scratch, ec);
  REQUIRE(std::filesystem::create_directories(scratch, ec));
  REQUIRE_FALSE(ec);
  const std::string played = scratch + "/feature_film.mkv";
  std::filesystem::copy_file(media, played,
                             std::filesystem::copy_options::overwrite_existing, ec);
  REQUIRE_FALSE(ec);
  const auto chat = test_servers::startChatServer(scratch, "status500", "sembed-chat-key");
  REQUIRE(chat.pid > 0);

  // Same private-display scheme as the other X11 cases; cases run serially.
  const std::string suffix = std::to_string(70 + (::getpid() % 25));
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
    MESSAGE("Xvfb failed to start; skipping the embedded-translate window test");
    ::kill(xvfb, SIGTERM);
    ::waitpid(xvfb, nullptr, 0);
    return;
  }

  pid_t injector = ::fork();
  REQUIRE(injector >= 0);
  if (injector == 0) {
    ::execlp("python3", "python3", "-c", kX11InjectorScript, display.c_str(),
             "soar", "subsembed", played.c_str(), audio_only.c_str(),
             static_cast<char*>(nullptr));
    _exit(127);
  }

  const std::string state_home = "/tmp/soar_subsembed_xdg_" + std::to_string(::getpid());
  ::mkdir(state_home.c_str(), 0755);  // EEXIST from a prior run is fine
  const ScopedEnv xdg_env("XDG_STATE_HOME", state_home.c_str());
  // The embedded bitmap font pins widget metrics so the injector's
  // coordinates mean the same thing on every machine.
  const ScopedEnv bitmap_font_env("SOAR_UI_BITMAP_FONT", "1");
  const ScopedEnv display_env("DISPLAY", display.c_str());
  const ScopedEnv audio_env("SDL_AUDIODRIVER", "dummy");
  const ScopedEnv tr_endpoint_env("SOAR_TRANSLATE_ENDPOINT", chat.base_url.c_str());
  const ScopedEnv tr_key_env("SOAR_TRANSLATE_API_KEY", "sembed-chat-key");
  const ScopedEnv tr_model_env("SOAR_TRANSLATE_MODEL", "sembed-model");
  const ScopedEnv tr_lang_env("SOAR_TRANSLATE_LANGUAGE", "Klingon");
  // Ctor arm: the translator's timeout override (no SOAR_SUBTITLE_*
  // endpoint in this run — the menu offers no [download] section).
  const ScopedEnv tr_timeout_env("SOAR_TRANSLATE_TIMEOUT_MS", "2500");
  const auto run = runCli({"--backend=ffmpeg", played});

  std::printf("x11 embedded-translate window test: cli exit=%d, output:\n%s\n",
              run.exit_code, run.output.c_str());

  ::waitpid(injector, nullptr, 0);
  ::kill(xvfb, SIGTERM);
  ::waitpid(xvfb, nullptr, 0);

  if (run.output.find("SDL_CreateRenderer failed") != std::string::npos) {
    MESSAGE("no accelerated renderer under this X server; skipping");
    std::filesystem::remove_all(scratch, ec);
    return;
  }
  REQUIRE(run.exit_code == 0);
  CHECK(run.output.find("event: state=2") != std::string::npos);
  CHECK(run.output.find("event: state=1") != std::string::npos);
  // The export ran: the numbered batch reached the chat fixture, which
  // logs every POST before answering 500 (the fixture's own record of the
  // traffic; the product never logs the key itself).
  {
    std::ifstream log(scratch + "/requests.log");
    REQUIRE(log.good());
    std::string line;
    bool found = false;
    while (std::getline(log, line)) {
      if (line.find("sembed-model") != std::string::npos) {
        found = true;
        CHECK(line.find("Bearer sembed-chat-key") != std::string::npos);
        CHECK(line.find("1. Hello") != std::string::npos);
      }
    }
    CHECK(found);
  }
  // Neither attempt loaded a track: a successful translate would have
  // landed on an external id past the container's stream range (the subs
  // fixture's nb_streams = 4, so the first external id is 4). The toast
  // arms are the observable contract; the selection trail must stay put.
  CHECK(run.output.find("selected(video=0,audio=1,sub=4)") == std::string::npos);
  std::filesystem::remove_all(scratch, ec);
#endif
#endif
#endif
}

TEST_CASE("windowed FFmpeg run keeps a dead catalog row and still translates the embedded track") {
#ifdef _WIN32
  MESSAGE("the X11 window test is POSIX-only; skipping");
  return;
#else
#ifndef SOAR_CLI_HAS_IMGUI
  MESSAGE("app built without the ImGui overlay; skipping the catalog-404 window test");
  return;
#else
  if (std::getenv("SOAR_TEST_X11") == nullptr) {
    MESSAGE("SOAR_TEST_X11 not set; skipping the catalog-404 window test");
    return;
  }
#ifndef SOAR_WITH_FFMPEG
  MESSAGE("no FFmpeg backend; skipping the catalog-404 window test");
  return;
#else
  std::string media;
  if (!envMediaPath("SOAR_TEST_SUBS_MEDIA", media)) {
    MESSAGE("SOAR_TEST_SUBS_MEDIA not set; skipping the catalog-404 window test");
    return;
  }
  if (std::system("command -v Xvfb >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1 && "
                  "python3 -c 'from Xlib.ext import xtest' >/dev/null 2>&1") != 0) {
    MESSAGE("Xvfb or python3-xlib missing; skipping the catalog-404 window test");
    return;
  }

  // A catalog row whose URL 404s, and a live translator behind the same
  // combo: pick 1 hits [download] and fetch-fails (nothing loaded, the row
  // still offered on reopen); pick 2 hits [translate] on the embedded srt
  // row — export, batch chat over the ok fixture, store, load. The server
  // reads catalog.tsv per request, so it starts before the file exists.
  const std::string scratch = "/tmp/soar_subsd404_x11_" + std::to_string(::getpid());
  std::error_code ec;
  std::filesystem::remove_all(scratch, ec);
  REQUIRE(std::filesystem::create_directories(scratch, ec));
  REQUIRE_FALSE(ec);
  const std::string played = scratch + "/feature_film.mkv";
  std::filesystem::copy_file(media, played,
                             std::filesystem::copy_options::overwrite_existing, ec);
  REQUIRE_FALSE(ec);
  const auto srv = test_servers::startSubtitleServer(scratch, "catalog", "subsd404-key");
  REQUIRE(srv.pid > 0);
  {
    std::ofstream catalog(scratch + "/catalog.tsv", std::ios::binary);
    REQUIRE(catalog.good());
    catalog << "# size hash name\n"
            << srv.base_url << "/dl/missing.en.srt\ten\tDead catalog row\tsrt\n";
  }
  const auto chat = test_servers::startChatServer(scratch, "ok", "subsd404-chat-key");
  REQUIRE(chat.pid > 0);

  // Same private-display scheme as the other X11 cases; cases run serially.
  const std::string suffix = std::to_string(70 + (::getpid() % 25));
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
    MESSAGE("Xvfb failed to start; skipping the catalog-404 window test");
    ::kill(xvfb, SIGTERM);
    ::waitpid(xvfb, nullptr, 0);
    return;
  }

  pid_t injector = ::fork();
  REQUIRE(injector >= 0);
  if (injector == 0) {
    ::execlp("python3", "python3", "-c", kX11InjectorScript, display.c_str(),
             "soar", "subsdl404", static_cast<char*>(nullptr));
    _exit(127);
  }

  const std::string state_home = "/tmp/soar_subsd404_xdg_" + std::to_string(::getpid());
  ::mkdir(state_home.c_str(), 0755);  // EEXIST from a prior run is fine
  const ScopedEnv xdg_env("XDG_STATE_HOME", state_home.c_str());
  // The embedded bitmap font pins widget metrics so the injector's
  // coordinates mean the same thing on every machine.
  const ScopedEnv bitmap_font_env("SOAR_UI_BITMAP_FONT", "1");
  const ScopedEnv display_env("DISPLAY", display.c_str());
  const ScopedEnv audio_env("SDL_AUDIODRIVER", "dummy");
  const ScopedEnv endpoint_env("SOAR_SUBTITLE_ENDPOINT", (srv.base_url + "/search").c_str());
  const ScopedEnv key_env("SOAR_SUBTITLE_API_KEY", "subsd404-key");
  // Ctor arm: the fetch timeout override.
  const ScopedEnv sub_timeout_env("SOAR_SUBTITLE_TIMEOUT_MS", "3000");
  const ScopedEnv tr_endpoint_env("SOAR_TRANSLATE_ENDPOINT", chat.base_url.c_str());
  const ScopedEnv tr_key_env("SOAR_TRANSLATE_API_KEY", "subsd404-chat-key");
  const ScopedEnv tr_model_env("SOAR_TRANSLATE_MODEL", "subsd404-model");
  const ScopedEnv tr_lang_env("SOAR_TRANSLATE_LANGUAGE", "Klingon");
  const auto run = runCli({"--backend=ffmpeg", played});

  std::printf("x11 catalog-404 window test: cli exit=%d, output:\n%s\n", run.exit_code,
              run.output.c_str());

  ::waitpid(injector, nullptr, 0);
  ::kill(xvfb, SIGTERM);
  ::waitpid(xvfb, nullptr, 0);

  if (run.output.find("SDL_CreateRenderer failed") != std::string::npos) {
    MESSAGE("no accelerated renderer under this X server; skipping");
    std::filesystem::remove_all(scratch, ec);
    return;
  }
  REQUIRE(run.exit_code == 0);
  CHECK(run.output.find("event: state=2") != std::string::npos);
  CHECK(run.output.find("event: state=1") != std::string::npos);
  // The translate loaded on the first external id (nb_streams = 4): the
  // dead download never consumed one, so id 4 is the translation and no
  // id 5 exists anywhere in the trail — [download] failed and [translate]
  // succeeded, in that order, or the numbers would differ.
  const std::size_t translated_on = run.output.find("selected(video=0,audio=1,sub=4)");
  CHECK(translated_on != std::string::npos);
  CHECK(run.output.find("selected(video=0,audio=1,sub=5)") == std::string::npos);
  // The batch reached the chat fixture: the export ran over loopback HTTP
  // and the translator answered.
  {
    std::ifstream log(scratch + "/requests.log");
    REQUIRE(log.good());
    std::string line;
    bool found = false;
    while (std::getline(log, line)) {
      if (line.find("subsd404-model") != std::string::npos) {
        found = true;
        CHECK(line.find("1. Hello") != std::string::npos);
      }
    }
    CHECK(found);
  }
  // Nothing after the pick may have turned subtitles back off — a stray
  // Off pick from a mis-aimed click would show up here.
  if (translated_on != std::string::npos) {
    CHECK(run.output.find("selected(video=0,audio=1,sub=-1)", translated_on) ==
          std::string::npos);
  }
  std::filesystem::remove_all(scratch, ec);
#endif
#endif
#endif
}

TEST_CASE("windowed FFmpeg run refuses a codec-broken subtitle track from the menu and the C cycle") {
#ifdef _WIN32
  MESSAGE("the X11 window test is POSIX-only; skipping");
  return;
#else
#ifndef SOAR_CLI_HAS_IMGUI
  MESSAGE("app built without the ImGui overlay; skipping the broken-track window test");
  return;
#else
  if (std::getenv("SOAR_TEST_X11") == nullptr) {
    MESSAGE("SOAR_TEST_X11 not set; skipping the broken-track window test");
    return;
  }
#ifndef SOAR_WITH_FFMPEG
  MESSAGE("no FFmpeg backend; skipping the broken-track window test");
  return;
#else
  std::string media;
  if (!envMediaPath("SOAR_TEST_SUBS_DUAL", media)) {
    MESSAGE("SOAR_TEST_SUBS_DUAL not set; skipping the broken-track window test");
    return;
  }
  if (std::system("command -v Xvfb >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1 && "
                  "python3 -c 'from Xlib.ext import xtest' >/dev/null 2>&1") != 0) {
    MESSAGE("Xvfb or python3-xlib missing; skipping the broken-track window test");
    return;
  }

  // Same-length CodecID patch on a copy (S_TEXT/ASS -> S_TEXT/XXX, the
  // backend suite's patch): the container still opens on its healthy
  // default subrip stream, the second subtitle track keeps its row in the
  // menu — but selecting it fails at the backend (switch-failed toast),
  // and C's cycle walks onto the same broken track after first selecting
  // the good one. The patched codec also denies the track a [translate]
  // row: the translatable scan's not-a-text-codec arm, which is why the
  // translate envs are configured for this run's menu.
  const std::string scratch = "/tmp/soar_subscycle_x11_" + std::to_string(::getpid());
  std::error_code ec;
  std::filesystem::remove_all(scratch, ec);
  REQUIRE(std::filesystem::create_directories(scratch, ec));
  REQUIRE_FALSE(ec);
  std::string bytes;
  {
    std::ifstream in(media, std::ios::binary);
    REQUIRE(in);
    std::ostringstream ss;
    ss << in.rdbuf();
    bytes = ss.str();
  }
  const std::size_t codec_id = bytes.find("S_TEXT/ASS");
  REQUIRE(codec_id != std::string::npos);
  bytes.replace(codec_id, 10, "S_TEXT/XXX");
  const std::string played = scratch + "/broken.mkv";
  {
    std::ofstream out(played, std::ios::binary);
    REQUIRE(out.good());
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  // Env honesty for the translate config: the [translate] row is offered
  // for the healthy srt track (never picked), and the broken codec is
  // filtered out — the menu build is the assertion surface.
  const auto chat = test_servers::startChatServer(scratch, "ok", "subscycle-chat-key");
  REQUIRE(chat.pid > 0);

  // Same private-display scheme as the other X11 cases; cases run serially.
  const std::string suffix = std::to_string(70 + (::getpid() % 25));
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
    MESSAGE("Xvfb failed to start; skipping the broken-track window test");
    ::kill(xvfb, SIGTERM);
    ::waitpid(xvfb, nullptr, 0);
    return;
  }

  pid_t injector = ::fork();
  REQUIRE(injector >= 0);
  if (injector == 0) {
    ::execlp("python3", "python3", "-c", kX11InjectorScript, display.c_str(),
             "soar", "subcycle", static_cast<char*>(nullptr));
    _exit(127);
  }

  const std::string state_home = "/tmp/soar_subscycle_xdg_" + std::to_string(::getpid());
  ::mkdir(state_home.c_str(), 0755);  // EEXIST from a prior run is fine
  const ScopedEnv xdg_env("XDG_STATE_HOME", state_home.c_str());
  // The embedded bitmap font pins widget metrics so the injector's
  // coordinates mean the same thing on every machine.
  const ScopedEnv bitmap_font_env("SOAR_UI_BITMAP_FONT", "1");
  const ScopedEnv display_env("DISPLAY", display.c_str());
  const ScopedEnv audio_env("SDL_AUDIODRIVER", "dummy");
  const ScopedEnv tr_endpoint_env("SOAR_TRANSLATE_ENDPOINT", chat.base_url.c_str());
  const ScopedEnv tr_key_env("SOAR_TRANSLATE_API_KEY", "subscycle-chat-key");
  const ScopedEnv tr_model_env("SOAR_TRANSLATE_MODEL", "subscycle-model");
  const ScopedEnv tr_lang_env("SOAR_TRANSLATE_LANGUAGE", "Klingon");
  const auto run = runCli({"--backend=ffmpeg", played});

  std::printf("x11 broken-track window test: cli exit=%d, output:\n%s\n", run.exit_code,
              run.output.c_str());

  ::waitpid(injector, nullptr, 0);
  ::kill(xvfb, SIGTERM);
  ::waitpid(xvfb, nullptr, 0);

  if (run.output.find("SDL_CreateRenderer failed") != std::string::npos) {
    MESSAGE("no accelerated renderer under this X server; skipping");
    std::filesystem::remove_all(scratch, ec);
    return;
  }
  REQUIRE(run.exit_code == 0);
  CHECK(run.output.find("event: state=2") != std::string::npos);
  CHECK(run.output.find("event: state=1") != std::string::npos);
  // The dual fixture's subrip stream is id 2 and the patched one id 3.
  // Windowed startup selects nothing (the auto-select lives in the
  // headless flow), so the first C press selects the healthy track — the
  // selection trail must show it — and the broken track must never show
  // up as selected anywhere: its menu pick and the second C both fail at
  // the backend and leave the selection where it was.
  const std::size_t healthy_on = run.output.find("selected(video=0,audio=1,sub=2)");
  CHECK(healthy_on != std::string::npos);
  CHECK(run.output.find("selected(video=0,audio=1,sub=3)") == std::string::npos);
  std::filesystem::remove_all(scratch, ec);
#endif
#endif
#endif
}

TEST_CASE("windowed FFmpeg run walks the play queue end to end") {
#ifdef _WIN32
  MESSAGE("the X11 window test is POSIX-only; skipping");
  return;
#else
#ifndef SOAR_CLI_HAS_IMGUI
  // The queue UI (P overlay, its buttons) is an ImGui widget; the
  // bare-SDL build has no HUD to drive.
  MESSAGE("app built without the ImGui overlay; skipping the playlist window test");
  return;
#else
  if (std::getenv("SOAR_TEST_X11") == nullptr) {
    MESSAGE("SOAR_TEST_X11 not set; skipping the playlist window test");
    return;
  }
#ifndef SOAR_WITH_FFMPEG
  MESSAGE("no FFmpeg backend; skipping the playlist window test");
  return;
#else
  std::string first;
  if (!envMediaPath("SOAR_TEST_AUDIO_ONLY", first)) {
    MESSAGE("SOAR_TEST_AUDIO_ONLY not set; skipping the playlist window test");
    return;
  }
  std::string second;
  if (!envMediaPath("SOAR_TEST_SUBS_MEDIA", second)) {
    MESSAGE("SOAR_TEST_SUBS_MEDIA not set; skipping the playlist window test");
    return;
  }
  if (std::system("command -v Xvfb >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1 && "
                  "python3 -c 'from Xlib.ext import xtest' >/dev/null 2>&1") != 0) {
    MESSAGE("Xvfb or python3-xlib missing; skipping the playlist window test");
    return;
  }

  // Same private-display scheme as the other X11 cases; cases run serially.
  const std::string suffix = std::to_string(70 + (::getpid() % 25));
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
    MESSAGE("Xvfb failed to start; skipping the playlist window test");
    ::kill(xvfb, SIGTERM);
    ::waitpid(xvfb, nullptr, 0);
    return;
  }

  // All three queue entries are staged as siblings in one scratch
  // directory. The overlay's folder tree view (v0.2.1) kicks in as soon
  // as the queue spans two parent directories, and its extra tree-node
  // rows would move every row the injector clicks (probed against the
  // flat layout). One parent keeps that layout; symlinks keep the
  // fixtures' track-count identities, and the dead sibling is simply a
  // path that does not exist. The CLI also tees its output to a file so
  // the injector can drive its clicks off observed playback state
  // (docs note in the injector's playlist branch) — hence this block
  // lives before the fork. .cache pins the file picker's sorted rows the
  // same way the dir-picker tour pins its own HOME (a dot-directory
  // always sorts first), and HOME points at the scratch tree so the
  // "Add files..." picker of the file-mode walkthrough opens right on it.
  const std::string queue_dir = "/tmp/soar_playlist_queue_" + std::to_string(::getpid());
  std::error_code queue_ec;
  std::filesystem::remove_all(queue_dir, queue_ec);
  REQUIRE(std::filesystem::create_directories(queue_dir, queue_ec));
  REQUIRE(std::filesystem::create_directories(queue_dir + "/.cache", queue_ec));
  const std::string first_q = queue_dir + "/one.mkv";
  const std::string second_q = queue_dir + "/two.mkv";
  const std::string dead_q = queue_dir + "/missing.mp4";
  std::filesystem::create_symlink(first, first_q, queue_ec);
  REQUIRE(!queue_ec);
  std::filesystem::create_symlink(second, second_q, queue_ec);
  REQUIRE(!queue_ec);
  const std::string cli_log = queue_dir + "/cli.log";

  pid_t injector = ::fork();
  REQUIRE(injector >= 0);
  if (injector == 0) {
    ::execlp("python3", "python3", "-c", kX11InjectorScript, display.c_str(),
             "soar", "playlist", cli_log.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }

  // Keep the run's MRU entries out of the real user state dir.
  const std::string state_home = "/tmp/soar_playlist_xdg_" + std::to_string(::getpid());
  ::mkdir(state_home.c_str(), 0755);  // EEXIST from a prior run is fine
  const ScopedEnv xdg_env("XDG_STATE_HOME", state_home.c_str());
  // $HOME is the file picker's start directory (openDirPicker's ladder),
  // so point it at the scratch tree: the "Add files..." walkthrough ticks
  // one.mkv and two.mkv straight off it instead of the real home.
  const ScopedEnv home_env("HOME", queue_dir.c_str());
  // The embedded bitmap font pins widget metrics so the injector's
  // coordinates mean the same thing on every machine.
  const ScopedEnv bitmap_font_env("SOAR_UI_BITMAP_FONT", "1");
  const ScopedEnv display_env("DISPLAY", display.c_str());
  const ScopedEnv audio_env("SDL_AUDIODRIVER", "dummy");
  // The first positional plays; the second queues behind it (mpv's
  // multi-argument semantics). Both fixtures run 6s but expose different
  // track counts, so every queue step is visible in the media-info trail
  // without any screenshots. The third path does not exist: it only gives
  // the queue a dead entry, whose failed open (n onto it) and removal are
  // part of the scripted tour.
  const auto run = runCli({"--backend=ffmpeg", first_q, second_q, dead_q}, 600, cli_log);

  std::printf("x11 playlist window test: cli exit=%d, output:\n%s\n", run.exit_code,
              run.output.c_str());

  ::waitpid(injector, nullptr, 0);
  ::kill(xvfb, SIGTERM);
  ::waitpid(xvfb, nullptr, 0);

  if (run.output.find("SDL_CreateRenderer failed") != std::string::npos) {
    MESSAGE("no accelerated renderer under this X server; skipping");
    return;
  }
  REQUIRE(run.exit_code == 0);
  CHECK(run.output.find("event: state=2") != std::string::npos);
  // The queue timeline, launch included — each open publishes MediaInfo,
  // so the trail reads as the fixture identities in play order (audio_only
  // is the 1-track fixture, subs_media the 3-track one):
  //   1 (launch) 3 (manual n) 1 (manual Shift+N) 3 (auto-advance at EOF)
  //   1 (Loop::All wrap) 1 (single-entry replay under Loop::One).
  // The failed open onto the missing entry closes the player without
  // publishing a content media-info, and the removals plus the
  // empty-queue n produce nothing either.
  // Each find resumes past the previous match (start + needle length):
  // searching from the match start itself would re-match the same line
  // and let the next step silently alias the previous occurrence.
  const std::string kT1 = "tracks=1 selected";
  const std::string kT3 = "tracks=3 selected";
  const std::size_t launch = run.output.find(kT1);
  CHECK(launch != std::string::npos);
  const std::size_t manual_next = run.output.find(kT3, launch + kT1.size());
  CHECK(manual_next != std::string::npos);
  const std::size_t manual_prev = run.output.find(kT1, manual_next + kT3.size());
  CHECK(manual_prev != std::string::npos);
  const std::size_t auto_advance = run.output.find(kT3, manual_prev + kT1.size());
  CHECK(auto_advance != std::string::npos);
  const std::size_t loop_wrap = run.output.find(kT1, auto_advance + kT3.size());
  CHECK(loop_wrap != std::string::npos);
  const std::size_t replay = run.output.find(kT1, loop_wrap + kT1.size());
  CHECK(replay != std::string::npos);
  // The removed entry never comes back: after the queue dropped to one
  // entry, no later open may surface the 3-track fixture again.
  if (replay != std::string::npos) {
    CHECK(run.output.find(kT3, replay + kT1.size()) == std::string::npos);
  }
  // The file-mode walkthrough (todo.md 原型对齐批): with the queue empty,
  // "Add 2 files" imports one.mkv + two.mkv and auto-plays the first — a
  // seventh media-info marker, the 1-track fixture again. The two-click
  // clear then wipes the queue ~1s into that replay, so its EOF finds an
  // emptied queue: no further open of either fixture.
  const std::size_t picked = run.output.find(kT1, replay + kT1.size());
  CHECK(picked != std::string::npos);
  if (picked != std::string::npos) {
    const std::size_t after_pick = picked + kT1.size();
    CHECK(run.output.find(kT3, after_pick) == std::string::npos);
    CHECK(run.output.find(kT1, after_pick) == std::string::npos);
  }
  // The pick's and the clear's toasts. "Imported" is the was-empty
  // branch; exactly one clear toast means the arm click really only
  // armed (the injector's wait-no guards the same thing from the other
  // side, exit 6).
  CHECK(run.output.find("event: toast=Imported 2 files") != std::string::npos);
  const std::string kCleared = "event: toast=Playlist cleared";
  const std::size_t cleared = run.output.find(kCleared);
  CHECK(cleared != std::string::npos);
  if (cleared != std::string::npos) {
    CHECK(run.output.find(kCleared, cleared + kCleared.size()) == std::string::npos);
  }
  std::filesystem::remove_all(queue_dir, queue_ec);
#endif
#endif
#endif
}

TEST_CASE("windowed FFmpeg run imports folders through the dir picker and plays a tree group") {
#ifdef _WIN32
  MESSAGE("the X11 window test is POSIX-only; skipping");
  return;
#else
#ifndef SOAR_CLI_HAS_IMGUI
  // The dir picker and the playlist tree are ImGui widgets; the bare-SDL
  // build has neither, so there is nothing to drive.
  MESSAGE("app built without the ImGui overlay; skipping the dir-picker window test");
  return;
#else
  if (std::getenv("SOAR_TEST_X11") == nullptr) {
    MESSAGE("SOAR_TEST_X11 not set; skipping the dir-picker window test");
    return;
  }
#ifndef SOAR_WITH_FFMPEG
  MESSAGE("no FFmpeg backend; skipping the dir-picker window test");
  return;
#else
  std::string first;
  if (!envMediaPath("SOAR_TEST_AUDIO_ONLY", first)) {
    MESSAGE("SOAR_TEST_AUDIO_ONLY not set; skipping the dir-picker window test");
    return;
  }
  std::string second;
  if (!envMediaPath("SOAR_TEST_SUBS_MEDIA", second)) {
    MESSAGE("SOAR_TEST_SUBS_MEDIA not set; skipping the dir-picker window test");
    return;
  }
  if (std::system("command -v Xvfb >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1 && "
                  "python3 -c 'from Xlib.ext import xtest' >/dev/null 2>&1") != 0) {
    MESSAGE("Xvfb or python3-xlib missing; skipping the dir-picker window test");
    return;
  }

  // Same private-display scheme as the other X11 cases; cases run serially.
  const std::string suffix = std::to_string(70 + (::getpid() % 25));
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
    MESSAGE("Xvfb failed to start; skipping the dir-picker window test");
    ::kill(xvfb, SIGTERM);
    ::waitpid(xvfb, nullptr, 0);
    return;
  }

  // The picker browses $HOME, so the tour stages one. ".cache" pins the
  // sorted row order (a dot-directory always sorts first, so whatever the
  // app itself drops into HOME cannot shift the rows the injector clicks).
  // media_a keeps one fixture at its top and the other a level down: the
  // "Include subfolders" toggle is load-bearing, a flat import would queue
  // only a1 and starve the a2 marker. media_b holds one fixture for the
  // sibling-folder import and the tree view's second group. The state
  // home starts empty, so the first Ctrl+O lands on HOME (no recents yet)
  // and the second one shows the section the imports grew.
  const std::string home_dir = "/tmp/soar_dirpick_home_" + std::to_string(::getpid());
  const std::string state_home = "/tmp/soar_dirpick_xdg_" + std::to_string(::getpid());
  std::error_code fs_ec;
  std::filesystem::remove_all(home_dir, fs_ec);
  std::filesystem::remove_all(state_home, fs_ec);
  // state_home must exist before runCli: the tee file lands there and the
  // fopen is silent when the directory is missing (the app only creates
  // the soar/ subtree later, on its first recents save).
  REQUIRE(std::filesystem::create_directories(state_home, fs_ec));
  REQUIRE(std::filesystem::create_directories(home_dir + "/.cache", fs_ec));
  REQUIRE(std::filesystem::create_directories(home_dir + "/media_a/deep", fs_ec));
  REQUIRE(std::filesystem::create_directories(home_dir + "/media_b", fs_ec));
  std::filesystem::create_symlink(first, home_dir + "/media_a/a1.mkv", fs_ec);
  REQUIRE(!fs_ec);
  std::filesystem::create_symlink(second, home_dir + "/media_a/deep/a2.mkv", fs_ec);
  REQUIRE(!fs_ec);
  std::filesystem::create_symlink(first, home_dir + "/media_b/b1.mkv", fs_ec);
  REQUIRE(!fs_ec);
  const std::string cli_log = state_home + "/cli.log";

  pid_t injector = ::fork();
  REQUIRE(injector >= 0);
  if (injector == 0) {
    ::execlp("python3", "python3", "-c", kX11InjectorScript, display.c_str(),
             "soar", "dirpick", cli_log.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }

  // The picker's start-directory ladder reads HOME and the recents write
  // under XDG_STATE_HOME — both must point into the scratch tree (and the
  // run's writes must stay out of the real user state).
  const ScopedEnv home_env("HOME", home_dir.c_str());
  const ScopedEnv xdg_env("XDG_STATE_HOME", state_home.c_str());
  // The embedded bitmap font pins widget metrics so the injector's
  // coordinates mean the same thing on every machine.
  const ScopedEnv bitmap_font_env("SOAR_UI_BITMAP_FONT", "1");
  const ScopedEnv display_env("DISPLAY", display.c_str());
  const ScopedEnv audio_env("SDL_AUDIODRIVER", "dummy");
  // No positional: the run starts on the poster, and the first content
  // open comes from the folder import itself.
  const auto run = runCli({"--gui", "--backend=ffmpeg"}, 600, cli_log);

  std::printf("x11 dir-picker window test: cli exit=%d, output:\n%s\n", run.exit_code,
              run.output.c_str());

  ::waitpid(injector, nullptr, 0);
  ::kill(xvfb, SIGTERM);
  ::waitpid(xvfb, nullptr, 0);

  if (run.output.find("SDL_CreateRenderer failed") != std::string::npos) {
    MESSAGE("no accelerated renderer under this X server; skipping");
    std::filesystem::remove_all(home_dir, fs_ec);
    std::filesystem::remove_all(state_home, fs_ec);
    return;
  }
  REQUIRE(run.exit_code == 0);
  CHECK(run.output.find("event: state=2") != std::string::npos);
  // The tour's media-info trail — the fixtures are told apart by track
  // count (audio_only=1, subs_media=3), so the import semantics read
  // straight off it:
  //   1  the recursive import of media_a auto-played a1.mkv
  //   3  a1's EOF advanced into the deeper a2.mkv — the recursive import
  //      really queued both files
  //   1  "Play group" on the media_b tree group opened b1.mkv
  // Nothing may follow: the second import was append-only (a restart
  // would have opened b1 right there, adding a fourth marker), and the
  // final folder-node collapse click must not have landed on a row.
  // Each find resumes past the previous match so no step aliases the
  // previous occurrence (same shape as the queue-walk case above).
  const std::string kT1 = "tracks=1 selected";
  const std::string kT3 = "tracks=3 selected";
  const std::size_t import_open = run.output.find(kT1);
  CHECK(import_open != std::string::npos);
  const std::size_t advance = run.output.find(kT3, import_open + kT1.size());
  CHECK(advance != std::string::npos);
  const std::size_t group_open = run.output.find(kT1, advance + kT3.size());
  CHECK(group_open != std::string::npos);
  if (group_open != std::string::npos) {
    const std::size_t after = group_open + kT1.size();
    CHECK(run.output.find(kT3, after) == std::string::npos);
    CHECK(run.output.find(kT1, after) == std::string::npos);
  }
  // Both imports recorded their folder in the recent-dirs MRU under the
  // redirected state home (the import records even when nothing plays).
  std::ifstream recents(state_home + "/soar/recent-dirs.txt");
  if (!recents.good()) {
    FAIL("recent-dirs.txt was not written under XDG_STATE_HOME");
  } else {
    const std::string recents_txt((std::istreambuf_iterator<char>(recents)),
                                  std::istreambuf_iterator<char>());
    CHECK(recents_txt.find("/media_a") != std::string::npos);
    CHECK(recents_txt.find("/media_b") != std::string::npos);
  }
  std::filesystem::remove_all(home_dir, fs_ec);
  std::filesystem::remove_all(state_home, fs_ec);
#endif
#endif
#endif
}

TEST_CASE("windowed FFmpeg run cycles a dual-audio fixture with no subtitles") {
#ifdef _WIN32
  MESSAGE("the X11 window test is POSIX-only; skipping");
  return;
#else
#ifndef SOAR_CLI_HAS_IMGUI
  // Track cycling and the overlays are ImGui-side HUD behaviour; the
  // bare-SDL build has no HUD to drive.
  MESSAGE("app built without the ImGui overlay; skipping the audio-track window test");
  return;
#else
  if (std::getenv("SOAR_TEST_X11") == nullptr) {
    MESSAGE("SOAR_TEST_X11 not set; skipping the audio-track window test");
    return;
  }
#ifndef SOAR_WITH_FFMPEG
  MESSAGE("no FFmpeg backend; skipping the audio-track window test");
  return;
#else
  std::string media;
  if (!envMediaPath("SOAR_TEST_MEDIA", media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping the audio-track window test");
    return;
  }
  if (std::system("command -v Xvfb >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1 && "
                  "python3 -c 'from Xlib.ext import xtest' >/dev/null 2>&1") != 0) {
    MESSAGE("Xvfb or python3-xlib missing; skipping the audio-track window test");
    return;
  }

  // Same private-display scheme as the other X11 cases; cases run serially.
  const std::string suffix = std::to_string(70 + (::getpid() % 25));
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
    MESSAGE("Xvfb failed to start; skipping the audio-track window test");
    ::kill(xvfb, SIGTERM);
    ::waitpid(xvfb, nullptr, 0);
    return;
  }

  pid_t injector = ::fork();
  REQUIRE(injector >= 0);
  if (injector == 0) {
    ::execlp("python3", "python3", "-c", kX11InjectorScript, display.c_str(),
             "soar", "audiodrive", static_cast<char*>(nullptr));
    _exit(127);
  }

  // Keep the run's MRU entries out of the real user state dir.
  const std::string state_home = "/tmp/soar_recent_xdg_" + std::to_string(::getpid()) + "a";
  ::mkdir(state_home.c_str(), 0755);  // EEXIST from a prior run is fine
  const ScopedEnv xdg_env("XDG_STATE_HOME", state_home.c_str());
  // The embedded bitmap font pins widget metrics so the injector's
  // coordinates mean the same thing on every machine.
  const ScopedEnv bitmap_font_env("SOAR_UI_BITMAP_FONT", "1");
  const ScopedEnv display_env("DISPLAY", display.c_str());
  const ScopedEnv audio_env("SDL_AUDIODRIVER", "dummy");
  // Two audio streams, no subtitle stream: the fixture behind A's
  // wrap-around and the "no subtitle tracks" degradation.
  const auto run = runCli({"--backend=ffmpeg", media});

  std::printf("x11 audio-track window test: cli exit=%d, output:\n%s\n", run.exit_code,
              run.output.c_str());

  ::waitpid(injector, nullptr, 0);
  ::kill(xvfb, SIGTERM);
  ::waitpid(xvfb, nullptr, 0);

  if (run.output.find("SDL_CreateRenderer failed") != std::string::npos) {
    MESSAGE("no accelerated renderer under this X server; skipping");
    return;
  }
  REQUIRE(run.exit_code == 0);
  // One video stream plus the two audio ones, and no subtitle stream at
  // all (which is what the C press and the Subtitle Settings page run
  // into).
  CHECK(run.output.find("tracks=3 selected(video=0,audio=1,sub=-1)") != std::string::npos);
  CHECK(run.output.find("event: state=1") != std::string::npos);
  // The first A steps onto the second stream and the second wraps back
  // to the first, so the trail has to be 1 -> 2 -> 1 in that order.
  const std::size_t to_second = run.output.find("selected(video=0,audio=2,sub=-1)");
  CHECK(to_second != std::string::npos);
  const std::size_t wrapped = run.output.find("selected(video=0,audio=1,sub=-1)", to_second);
  CHECK(wrapped != std::string::npos);
  // No failed switch anywhere: a refusal would surface as an Error event
  // and leave the selection where it was.
  CHECK(run.output.find("event: error=") == std::string::npos);
#endif
#endif
#endif
}

TEST_CASE("windowed FFmpeg run paints embedded ASS through the libass overlay") {
  // The overlay block in the video-present path (AssRenderer frame size
  // handshake, renderAt per frame, overlay texture create/update/blend)
  // only runs when a real window presents frames AND the media's ASS
  // track was claimed by libass — a combination no headless suite
  // reaches. One quiet window run over the styled fixture is the smoke:
  // its two cues span the whole 4s media with no gap, so every presented
  // frame carries overlay work. The injector's fullscreen round-trip
  // changes the destination rectangle, which recreates the overlay
  // texture at the new geometry; the end-of-media vanish exercises the
  // overlay teardown, and an escape storm exits the run — the point is
  // the render loop, not input handling.
#ifdef _WIN32
  MESSAGE("the X11 window test is POSIX-only; skipping");
  return;
#else
  if (std::getenv("SOAR_TEST_X11") == nullptr) {
    MESSAGE("SOAR_TEST_X11 not set; skipping the ASS overlay window test");
    return;
  }
#ifndef SOAR_WITH_FFMPEG
  MESSAGE("no FFmpeg backend; skipping the ASS overlay window test");
  return;
#else
  std::string media;
  if (!envMediaPath("SOAR_TEST_STYLED_ASS_MEDIA", media)) {
    MESSAGE("SOAR_TEST_STYLED_ASS_MEDIA not set; skipping the ASS overlay window test");
    return;
  }
  if (std::system("command -v Xvfb >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1 && "
                  "python3 -c 'from Xlib.ext import xtest' >/dev/null 2>&1") != 0) {
    MESSAGE("Xvfb or python3-xlib missing; skipping the ASS overlay window test");
    return;
  }

  // Same private-display scheme as the other X11 cases; cases run serially.
  const std::string suffix = std::to_string(70 + (::getpid() % 25));
  const std::string display = ":" + suffix;
  const std::string socket = "/tmp/.X11-unix/X" + suffix;

  pid_t xvfb = ::fork();
  REQUIRE(xvfb >= 0);
  if (xvfb == 0) {
    // A screen larger than the window: the fullscreen round-trip must
    // change the destination rectangle, or the overlay texture would
    // never see a size change to recreate at.
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
    MESSAGE("Xvfb failed to start; skipping the ASS overlay window test");
    ::kill(xvfb, SIGTERM);
    ::waitpid(xvfb, nullptr, 0);
    return;
  }

  pid_t injector = ::fork();
  REQUIRE(injector >= 0);
  if (injector == 0) {
    ::execlp("python3", "python3", "-c", kX11InjectorScript, display.c_str(),
             "soar", "assdrive", static_cast<char*>(nullptr));
    _exit(127);
  }

  const ScopedEnv display_env("DISPLAY", display.c_str());
  const ScopedEnv audio_env("SDL_AUDIODRIVER", "dummy");
  const auto run = runCli({"--backend=ffmpeg", media});

  std::printf("x11 ASS overlay window test: cli exit=%d, output:\n%s\n", run.exit_code,
              run.output.c_str());

  ::waitpid(injector, nullptr, 0);
  ::kill(xvfb, SIGTERM);
  ::waitpid(xvfb, nullptr, 0);

  if (run.output.find("SDL_CreateRenderer failed") != std::string::npos) {
    MESSAGE("no accelerated renderer under this X server; skipping");
    return;
  }
  CHECK(run.exit_code == 0);
  // The backend opened and reached playing: the decode thread fed the
  // styled track and the present loop had live frames to blend over.
  CHECK(run.output.find("event: state=2") != std::string::npos);
  // No failed open, no decoder error: libass claiming the track must not
  // disturb the plain decode path.
  CHECK(run.output.find("event: error=") == std::string::npos);
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
  // The socket check must answer for OUR Xvfb: a stale Xvfb from an
  // earlier session can already own the pid-derived display, so the fork
  // dies on "Server is already active" while the stat succeeds against
  // the leftover's socket. A losing fork lands within moments; require
  // the process to still be alive after a short settle, else the window
  // would be managed by a foreign X server and the injected keys would
  // never reach the child.
  int xvfb_status = 0;
  bool xvfb_dead = false;
  for (int i = 0; i < 10 && !xvfb_dead; ++i) {
    ::usleep(100 * 1000);
    xvfb_dead = ::waitpid(xvfb, &xvfb_status, WNOHANG) == xvfb;
  }
  if (xvfb_dead) {
    MESSAGE("Xvfb failed to start; skipping the buffering window test");
    server.stop();
    return;
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
  // Child budget 1500s (same rationale as the subsdrive case): the
  // throttled source must play to the stall threshold in wall-clock time,
  // and under --coverage -O0 with the box shared, 600s has been observed
  // to run out before the child quits.
  const auto run = runCli({"--backend=ffmpeg", url}, 1500);

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
  // The injector now outlives the throttled server's second burst, so the
  // same arc must close: the Ended print in main.cpp runs when the burst
  // un-stalls the read.
  CHECK(run.output.find("event: buffering ended") != std::string::npos);
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

  // The readiness check must answer for OUR server: on a shared box the
  // pid-derived port can be held by someone else's listener, so connect
  // succeeds while the forked server dies on bind. A dying server lands
  // within moments; require the process to still be alive after a short
  // settle, else the child would play the fixture through a foreign
  // server and hang for its full budget.
  int server_status = 0;
  bool server_dead = false;
  for (int i = 0; i < 10 && !server_dead; ++i) {
    ::usleep(100 * 1000);
    server_dead = ::waitpid(server, &server_status, WNOHANG) == server;
  }
  if (server_dead) {
    MESSAGE("local HTTP server died on bind; port already taken; skipping");
    return;
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

TEST_CASE("hangDumpTail keeps the end and stays bounded") {
  // The retry sites print this tail so a single caught hang still lands
  // its in-binary dump in the run log (BUGS.md #4); the semantics here
  // are the whole string when short, else exactly the last max_bytes.
  CHECK(hangDumpTail("", 16) == "");
  CHECK(hangDumpTail("short", 16) == "short");
  CHECK(hangDumpTail("exactly16chars!!", 16) == "exactly16chars!!");
  CHECK(hangDumpTail("0123456789abcdefghij", 4) == "ghij");
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
