#include "ui_state.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>

#ifdef _WIN32
#  include <shlobj.h>
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

}  // namespace soar::app
