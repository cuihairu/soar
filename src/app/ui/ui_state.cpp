#include "ui_state.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>

#ifdef _WIN32
#  include <direct.h>  // _mkdir
#else
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace soar::app {

namespace {

std::chrono::milliseconds::rep toRep(std::chrono::milliseconds t) {
  return t.count();
}

// Creates dir and any missing parents (best effort; the caller's save() is
// what surfaces real failures). Mirrors HttpCache::ensureDir semantics.
void ensureDir(const std::string& dir) {
  for (std::size_t i = 1; i <= dir.size(); ++i) {
    if (i == dir.size() || dir[i] == '/') {
#ifdef _WIN32
      _mkdir(dir.substr(0, i).c_str());
#else
      ::mkdir(dir.substr(0, i).c_str(), 0755);
#endif
    }
  }
}

std::string parentDir(const std::string& path) {
  const std::size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

}  // namespace

std::string formatClock(std::chrono::milliseconds t) {
  if (t < std::chrono::milliseconds::zero()) t = std::chrono::milliseconds(0);
  const auto total = static_cast<std::uint64_t>(toRep(t) / 1000);
  const std::uint64_t secs = total % 60;
  const std::uint64_t mins = (total / 60) % 60;
  const std::uint64_t hours = total / 3600;
  char buf[32];
  if (hours > 0) {
    std::snprintf(buf, sizeof(buf), "%llu:%02llu:%02llu",
                  static_cast<unsigned long long>(hours),
                  static_cast<unsigned long long>(mins),
                  static_cast<unsigned long long>(secs));
  } else {
    std::snprintf(buf, sizeof(buf), "%02llu:%02llu",
                  static_cast<unsigned long long>(mins),
                  static_cast<unsigned long long>(secs));
  }
  return buf;
}

bool HudVisibility::visible(std::chrono::milliseconds now, bool pointer_over_hud,
                            bool seek_drag, bool overlay_open,
                            bool playback_paused) const {
  // Manual override first: a click that hides the bar hides it even while
  // paused (explicit intent), then pins, then the idle timer.
  if (force_ == Force::Hide) return false;
  if (force_ == Force::Show) return true;
  if (pointer_over_hud || seek_drag || overlay_open || playback_paused) {
    return true;
  }
  return now - last_activity_ < kHideDelay;
}

void RecentStore::load() {
  entries_.clear();
  std::ifstream in(path_);
  if (!in.good()) return;
  std::string line;
  while (std::getline(in, line)) {
    // Trim trailing CR (files edited on Windows) and surrounding spaces.
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
      line.pop_back();
    }
    std::size_t start = line.find_first_not_of(' ');
    if (start == std::string::npos) continue;
    line = line.substr(start);
    if (line.empty()) continue;
    // Deduplicate while loading: a hand-edited file may repeat entries and
    // the UI must not show one source twice.
    if (std::find(entries_.begin(), entries_.end(), line) == entries_.end()) {
      entries_.push_back(line);
    }
    if (entries_.size() >= kMaxEntries) break;
  }
}

bool RecentStore::add(const std::string& uri) {
  auto it = std::find(entries_.begin(), entries_.end(), uri);
  // On an empty list end()==begin(); only a *found* front entry counts as
  // "already first" (the move-to-front of a new item is a real change).
  const bool already_first = it != entries_.end() && it == entries_.begin();
  if (it != entries_.end()) entries_.erase(it);
  entries_.insert(entries_.begin(), uri);
  if (entries_.size() > kMaxEntries) {
    entries_.resize(kMaxEntries);
  }
  return !already_first;
}

bool RecentStore::save() const {
  const std::string dir = parentDir(path_);
  if (!dir.empty()) ensureDir(dir);
  const std::string tmp = path_ + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out.good()) return false;
    for (const auto& uri : entries_) {
      out << uri << '\n';
      if (!out.good()) return false;
    }
  }
#ifdef _WIN32
  // rename() fails when the target exists on Windows.
  std::remove(path_.c_str());
#endif
  return std::rename(tmp.c_str(), path_.c_str()) == 0;
}

std::string defaultRecentPath() {
  const char* dir = nullptr;
#ifdef _WIN32
  dir = std::getenv("APPDATA");
  if (dir && *dir) return std::string(dir) + "\\soar\\recent.txt";
  return "soar-recent.txt";
#else
  dir = std::getenv("XDG_STATE_HOME");
  if (dir && *dir) return std::string(dir) + "/soar/recent.txt";
  const char* home = std::getenv("HOME");
  if (home && *home) return std::string(home) + "/.local/state/soar/recent.txt";
  return "soar-recent.txt";
#endif
}

//=============================================================================
// Playlist queue (docs/mvp.md §2)
//=============================================================================

void PlaylistStore::add(std::string uri) {
  entries_.push_back(std::move(uri));
  played_.push_back(false);
  // A freshly opened file is what is playing; an empty list has no other
  // candidate for "current".
  if (current_ == kNone) current_ = entries_.size() - 1;
}

bool PlaylistStore::remove(std::size_t index) {
  if (index >= entries_.size()) return false;
  entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));
  played_.erase(played_.begin() + static_cast<std::ptrdiff_t>(index));
  if (entries_.empty()) {
    current_ = kNone;
    return true;
  }
  if (index < current_) --current_;
  // Removing the current entry leaves `current_` pointing at its successor;
  // removing the tail clamps back onto the new last entry.
  if (current_ >= entries_.size()) current_ = entries_.size() - 1;
  return true;
}

bool PlaylistStore::setCurrent(std::size_t index) {
  if (index >= entries_.size()) return false;
  current_ = index;
  return true;
}

void PlaylistStore::clear() {
  entries_.clear();
  played_.clear();
  current_ = kNone;
}

void PlaylistStore::setShuffle(bool on) {
  if (on == shuffle_) return;
  shuffle_ = on;
  // A toggle starts a fresh round either way: the flags describe the round
  // in progress, and switching order mid-round would leave a half-played
  // set behind.
  played_.assign(entries_.size(), false);
}

std::uint64_t PlaylistStore::draw() {
  // splitmix64: one fixed sequence per seed on every standard library.
  rng_state_ += 0x9E3779B97F4A7C15ULL;
  std::uint64_t z = rng_state_;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

std::size_t PlaylistStore::pickOther() {
  // Caller guarantees size() > 1 and a valid current_, so the +1 nudge
  // below can never wrap onto current_ itself.
  std::size_t pick = static_cast<std::size_t>(draw() % entries_.size());
  if (pick == current_) pick = (pick + 1) % entries_.size();
  return pick;
}

std::size_t PlaylistStore::next() {
  if (entries_.empty() || current_ == kNone) return kNone;
  if (shuffle_ && entries_.size() > 1) return pickOther();
  if (current_ + 1 < entries_.size()) return current_ + 1;
  return loop_ == Loop::Off ? kNone : 0;
}

std::size_t PlaylistStore::prev() {
  if (entries_.empty() || current_ == kNone) return kNone;
  if (shuffle_ && entries_.size() > 1) return pickOther();
  if (current_ > 0) return current_ - 1;
  return loop_ == Loop::Off ? kNone : entries_.size() - 1;
}

std::size_t PlaylistStore::advance() {
  if (entries_.empty() || current_ == kNone) return kNone;
  if (loop_ == Loop::One) return current_;
  if (!shuffle_) return next();

  // Shuffle walks one round without repeats: the entry that just finished
  // counts as played, so Loop::Off stops when every entry had a turn and
  // Loop::All opens the next round instead. The finished entry joins the
  // candidates for *this* pick only after the loop below has collected
  // them — marking it first would put it back into the running when the
  // round still has other entries left, and a shuffle that replays the
  // track it just finished is not a shuffle.
  played_[current_] = true;
  std::vector<std::size_t> candidates;
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    if (!played_[i]) candidates.push_back(i);
  }
  if (candidates.empty()) {
    // Every entry had a turn, including the one that just finished.
    candidates.push_back(current_);
    if (loop_ == Loop::Off) return kNone;
    played_.assign(entries_.size(), false);
    // Loop::All opens the next round: the finished entry is un-marked
    // again, but it stays out of this pick so the transition into a new
    // round is not a back-to-back repeat. A single-entry list falls
    // through to the replay below.
    for (std::size_t i = 0; i < entries_.size(); ++i) {
      if (i != current_) candidates.push_back(i);
    }
    if (candidates.empty()) return current_;
  }
  return candidates[static_cast<std::size_t>(draw() % candidates.size())];
}

}  // namespace soar::app
