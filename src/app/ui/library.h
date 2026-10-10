// Media library store (docs/mvp.md §3): watched folders, the scanned file
// list, and per-source playback history (the resume point) in one persisted
// record set. Pure logic over std::filesystem — no SDL or ImGui here on
// purpose, same contract as ui_state.h/dir_scan.h: the window layer owns
// rendering and input, this stays unit-testable without a display.
//
// One record set serves both halves of the feature on purpose: the resume
// point belongs to the source (its path/URI), not to the folder it was
// discovered through, so a file opened directly from a dialog carries its
// history into the library and a scanned file keeps its history across
// scans. Keys are the URI exactly as opened (http:// URLs included);
// torrent bridge URLs change port every session and are skipped by the
// caller, not here.

#ifndef SOAR_APP_UI_LIBRARY_H_
#define SOAR_APP_UI_LIBRARY_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace soar::app {

// One known source: a scanned file or anything opened at least once. All
// counters are plain integers — no std::chrono in the persisted shape, so
// the file format cannot drift with a chrono policy change.
struct LibraryEntry {
  std::string path;               // key: path/URI exactly as opened
  std::uintmax_t size = 0;        // bytes; 0 = unknown (URLs, not yet scanned)
  std::int64_t mtime = 0;         // opaque change token (see scan()); 0 = unknown
  std::int64_t position_ms = 0;   // resume point; 0 = watched or never played
  std::int64_t duration_ms = 0;   // learned at open; 0 = unknown
  std::int64_t last_played = 0;   // epoch seconds of the last open; 0 = never
  std::uint32_t play_count = 0;
};

inline bool operator==(const LibraryEntry& a, const LibraryEntry& b) {
  return a.path == b.path && a.size == b.size && a.mtime == b.mtime &&
         a.position_ms == b.position_ms && a.duration_ms == b.duration_ms &&
         a.last_played == b.last_played && a.play_count == b.play_count;
}

// Offline NFO scrape (docs/mvp.md §3, the sidecar half only): a sibling
// <name>.nfo next to a scanned media file can carry display metadata.
// Read once per scan into an in-memory cache — the sidecar file on disk is
// the source of truth (it is user-editable, so persisting a copy would
// only create a second place to drift), and direct-opened sources outside
// every watched folder simply have no scrape (no per-frame probing).
struct NfoInfo {
  std::string title;
  std::string plot;
};

// Resume policy (docs/todo.md 媒体库批): a position is worth resuming when
// it is past the intro-scale noise (5 s — headless probes and credits
// skips must not plant a resume), the duration is known, and the source is
// not effectively finished (within 5 s of the end — "resume" must not mean
// "replay the last five seconds"). Returns the position in ms, 0 = none.
std::int64_t resumePositionMs(const LibraryEntry& e);

class MediaLibrary {
 public:
  // Bound on the persisted entry count: a runaway download directory must
  // not grow the state file without limit. Entries sort by path, so the
  // kept set is the lexically-first — arbitrary but deterministic.
  static constexpr std::size_t kMaxEntries = 2000;

  explicit MediaLibrary(std::string store_path) : path_(std::move(store_path)) {}

  // Reads the store if present. A missing file is an empty library, not an
  // error; malformed lines degrade to skips (whatever parses, keeps) and
  // duplicate paths collapse last-wins, so a hand-edited file never wedges
  // the session.
  void load();
  bool save() const;

  // Watched folders. addFolder is idempotent (first position wins);
  // removeFolder reports whether the folder was actually watched.
  void addFolder(std::string dir);
  bool removeFolder(const std::string& dir);
  const std::vector<std::string>& folders() const { return folders_; }

  // Re-walks every watched folder recursively (listMediaFiles semantics:
  // media extensions only, deterministic byte-order) and rebuilds the
  // scanned half of the entry set. Known files keep their history fields
  // and refresh size/mtime; files deleted from a *readable* watched folder
  // are pruned; a watched folder that is itself missing or unreadable is
  // skipped untouched — an unmounted drive must not look like a mass
  // deletion. Entries whose path is under no watched folder (URLs, files
  // opened directly) always survive the scan.
  void scan();

  const std::vector<LibraryEntry>& entries() const { return entries_; }

  // The scrape gathered by the last scan(), or null when the file has no
  // readable <name>.nfo sidecar (or scan never ran / the file left the
  // watched set). The pointer is stable until the next scan().
  const NfoInfo* nfoFor(const std::string& path) const;

  // Null when the path was never seen.
  const LibraryEntry* find(const std::string& path) const;

  // Records an open: the entry is created on first sight, play_count
  // increments, last_played moves to `now_epoch`. Position/duration are
  // not touched here — an open starts a new playback, it does not confirm
  // the old one.
  void recordOpen(const std::string& path, std::int64_t now_epoch);

  // Progress write from the playback loop (throttled by the caller).
  // Creates the entry when missing — a progress write implies the source
  // was opened, so the history row should exist even if recordOpen raced
  // or was skipped.
  void recordProgress(const std::string& path, std::int64_t position_ms,
                      std::int64_t duration_ms);

  // Watched-to-end: the resume point clears (advance-to-next and a manual
  // replay both land here). No-op for an unknown path.
  void clearPosition(const std::string& path);

  // UI "forget": drops the entry entirely. False for an unknown path.
  bool removeEntry(const std::string& path);

 private:
  // Insert-or-find keeping entries_ sorted by path. The returned pointer
  // is valid until the next mutation of the vector.
  LibraryEntry* upsert(const std::string& path);

  std::string path_;
  std::vector<std::string> folders_;
  std::vector<LibraryEntry> entries_;  // sorted by path at all times
  std::map<std::string, NfoInfo> nfo_;  // rebuilt whole by each scan()
};

// Same state dir as recent.txt (XDG_STATE_HOME / APPDATA): <dir>/soar/
// library.txt. Overridable in tests exactly like the other stores — point
// XDG_STATE_HOME at a scratch tree.
std::string defaultLibraryPath();

}  // namespace soar::app

#endif  // SOAR_APP_UI_LIBRARY_H_
