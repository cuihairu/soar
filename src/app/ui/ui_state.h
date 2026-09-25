// Pure UI logic for the windowed player (docs/ui-design.md): the pieces that
// must behave deterministically and stay unit-testable without a display —
// the OSC auto-hide state machine, the recent-files store, the OSD toast,
// and time formatting. No SDL or ImGui headers here on purpose; the window
// layer (player_window.cpp) owns rendering and input plumbing.
//
// All timing functions take the current time as a parameter so tests inject
// their own clock (the suite never bets on wall time).

#ifndef SOAR_APP_UI_STATE_H_
#define SOAR_APP_UI_STATE_H_

#include <chrono>
#include <string>
#include <vector>

namespace soar::app {

// "mm:ss" below an hour, "h:mm:ss" above (IINA/mpv clock style). Negative
// input clamps to zero; anything past 99h keeps growing the hour digits.
std::string formatClock(std::chrono::milliseconds t);

// OSC auto-hide semantics (docs/ui-design.md §3.1): the bar is visible from
// the first frame, resets its idle timer on every input, hides after
// kHideDelay of quiet, and stays pinned while any instantaneous pin
// condition holds (pointer over the bar/its menus, seek drag in progress,
// an overlay open, or playback paused/stopped/errored).
class HudVisibility {
 public:
  static constexpr std::chrono::milliseconds kHideDelay{2500};  // IINA default

  // Manual override (the single-click toggle): explicit intent beats both
  // the idle timer and the pins; Force::None returns to timer control.
  enum class Force { None, Show, Hide };

  explicit HudVisibility(std::chrono::milliseconds now)
      : last_activity_(now) {}

  // Any user input (key, wheel, click) restarts the timer and hands
  // control back to it — a hidden bar reappears on the next activity.
  void noteActivity(std::chrono::milliseconds now) {
    last_activity_ = now;
    force_ = Force::None;
  }

  void setForce(Force f) { force_ = f; }
  Force force() const { return force_; }

  bool visible(std::chrono::milliseconds now, bool pointer_over_hud,
               bool seek_drag, bool overlay_open, bool playback_paused) const;

  std::chrono::milliseconds lastActivity() const { return last_activity_; }

 private:
  std::chrono::milliseconds last_activity_;
  Force force_ = Force::None;  // timer-driven from the first frame
};

// One-line OSD feedback ("Volume 75%", "1.50x", "Audio track 2") shown for
// kDuration, fading under the caller's control via age().
class Toast {
 public:
  static constexpr std::chrono::milliseconds kDuration{800};

  void show(std::string text, std::chrono::milliseconds now) {
    text_ = std::move(text);
    shown_at_ = now;
  }

  bool active(std::chrono::milliseconds now) const {
    return now >= shown_at_ && now - shown_at_ < kDuration;
  }

  // 0 at show time, 1 at expiry — the caller maps this to alpha (with the
  // last 25% reserved for the fade-out ramp).
  double age(std::chrono::milliseconds now) const {
    const auto since = now - shown_at_;
    const double t = static_cast<double>(since.count()) /
                     static_cast<double>(kDuration.count());
    return t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
  }

  const std::string& text() const { return text_; }

 private:
  std::string text_;
  std::chrono::milliseconds shown_at_{-kDuration};
};

// MRU list of opened sources (docs/ui-design.md §2): mpv.net remembers 15
// (its --recent-count default), IINA records recent files by default — the
// v0.1 cap matches. Persisted as one URI per line via tmp+rename (the same
// atomic-write shape as the HttpCache meta file).
class RecentStore {
 public:
  static constexpr std::size_t kMaxEntries = 15;

  explicit RecentStore(std::string path) : path_(std::move(path)) {}

  // Reads the file if present. A missing file is an empty list, not an
  // error; corrupt content degrades to whatever prefix parses (empty lines
  // and whitespace-only entries are dropped).
  void load();

  // Adds `uri` at the front (moving an existing entry), trims to
  // kMaxEntries, returns whether the stored list changed.
  bool add(const std::string& uri);

  const std::vector<std::string>& entries() const { return entries_; }

  bool save() const;

 private:
  std::string path_;
  std::vector<std::string> entries_;
};

// XDG state dir on POSIX (APPDATA on Windows): <dir>/soar/recent.txt.
std::string defaultRecentPath();

}  // namespace soar::app

#endif  // SOAR_APP_UI_STATE_H_
