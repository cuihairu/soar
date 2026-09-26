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
#include <cstdint>
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

// Playlist queue (docs/mvp.md §2: 追加/删除/下一首/上一首/循环/随机). Pure
// index arithmetic over a vector of URIs — the window owns "open the picked
// uri", this owns "which entry is next". No persistence in v0.2: a fresh
// launch starts from the file that was opened (mpv/IINA do not persist
// playlists either).
class PlaylistStore {
 public:
  static constexpr std::size_t kNone = static_cast<std::size_t>(-1);

  // Loop::One replays the current entry when playback ends (advance());
  // manual stepping treats One like All and wraps, so Next still moves.
  enum class Loop { Off, All, One };

  // Appends. An empty list adopts the new entry as current — a freshly
  // opened file is what is playing.
  void add(std::string uri);

  // Removes one entry; `current` follows the removal (earlier entries shift
  // it left, removing the current entry adopts its successor, an emptied
  // list leaves kNone). False for an out-of-range index.
  bool remove(std::size_t index);

  // Marks `index` as the playing entry. False (and unchanged) when out of
  // range, which includes the empty list.
  bool setCurrent(std::size_t index);

  void clear();

  std::size_t current() const { return current_; }
  bool empty() const { return entries_.empty(); }
  std::size_t size() const { return entries_.size(); }
  const std::vector<std::string>& entries() const { return entries_; }

  Loop loop() const { return loop_; }
  void setLoop(Loop mode) { loop_ = mode; }
  bool shuffle() const { return shuffle_; }
  // Enabling shuffle starts a fresh round (nothing counts as played yet).
  void setShuffle(bool on);

  // Pins the shuffle sequence: the picker is a splitmix64 step, never
  // std::uniform_int_distribution (whose sequence differs per standard
  // library), so the tests can assert exact picks.
  void reseed(std::uint64_t seed) { rng_state_ = seed; }

  // One step forward/back from `current`: shuffle (n > 1) picks another
  // pseudo-random entry instead of the neighbour, otherwise step with wrap
  // (Loop::All and Loop::One wrap, Loop::Off stops at the end). kNone means
  // nowhere to go (empty, unset, or exhausted under Loop::Off). Pure: the
  // caller adopts the returned index once that entry opens.
  std::size_t next();
  std::size_t prev();

  // Where to continue when the current entry finishes: Loop::One replays
  // it, sequential mode behaves like next(), and shuffle walks one round
  // without repeats — the round's played flags are marked here, so
  // Loop::Off stops once every entry has had a turn (manual next()/prev()
  // never mark: an explicit pick is its own round). Loop::All starts the
  // next round instead of stopping.
  std::size_t advance();

 private:
  // One splitmix64 step: the pick source for both the neighbour-free
  // shuffle step and the round picker (fixed constants, identical on every
  // standard library — std::uniform_int_distribution is not).
  std::uint64_t draw();
  std::size_t pickOther();

  std::vector<std::string> entries_;
  // Parallel to entries_: "already had a turn in this shuffle round".
  // Spliced on add/remove; only the shuffle path of advance() reads it.
  std::vector<bool> played_;
  std::size_t current_{kNone};
  Loop loop_{Loop::Off};
  bool shuffle_{false};
  std::uint64_t rng_state_{0x9E3779B97F4A7C15ULL};
};

// XDG state dir on POSIX (APPDATA on Windows): <dir>/soar/recent.txt.
std::string defaultRecentPath();

}  // namespace soar::app

#endif  // SOAR_APP_UI_STATE_H_
