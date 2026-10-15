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
// caller, not here. File paths are one key per file on disk: every entry
// point folds separators to '/' (normalizePath — lossless on Windows where
// '\' is never a filename character, a no-op on POSIX where it can be), so
// argv, dialog and scanned forms of the same source cannot fork into two
// rows with split history.

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

// A network metadata source (docs/mvp.md §3, the network half of the scrape
// line): what the library asks when a scanned file has no sidecar. The
// library keeps a non-owning pointer, null = sidecars only, so the core
// scraper stays out of this header and an unconfigured session never has to
// talk to one.
//
// Contract, mirroring the SubtitleProvider's: scrape() must never throw and
// must not hold up the caller forever - a source that is unreachable, empty
// or misconfigured returns false and the row simply keeps the file name.
class MetadataSource {
 public:
  virtual ~MetadataSource() = default;

  // True when something usable was scraped. `path` is the media file's path;
  // the source derives its own query (the file name stem) from it.
  virtual bool scrape(const std::string& path, NfoInfo& out) const = 0;
};

// The query a scanned file offers a network source: its name with the
// directory and the extension folded off, whitespace-trimmed. Pure, so the
// rule is unit-testable without a source behind it.
std::string scrapeQueryForPath(const std::string& path);

// Resume policy (docs/todo.md 媒体库批): a position is worth resuming when
// it is past the intro-scale noise (5 s — headless probes and credits
// skips must not plant a resume), the duration is known, and the source is
// not effectively finished (within 5 s of the end — "resume" must not mean
// "replay the last five seconds"). Returns the position in ms, 0 = none.
std::int64_t resumePositionMs(const LibraryEntry& e);

// Library sidebar buckets (docs/todo.md 原型对齐批): the same record set
// read as a state machine — what joined recently, what carries a resume
// point, what was watched to the end. Pure predicates, so the window layer
// stays a renderer and the bucket membership is unit-testable.
enum class LibraryCategory { All, Recent, Resume, Finished };

// Whether an entry belongs to the bucket. Recent reads the file's mtime:
// the store keeps no add-time, and a scanned file's content time is the
// closest acquisition signal there is (0 = unknown — URLs, sources never
// scanned — and those are never "recent"). The token's epoch is the
// platform's file_clock (libstdc++ dates from 2174, so current files
// carry negative tokens), so only 0 is the marker — never a sign test.
// Resume is the open flow's own policy (resumePositionMs). Finished is
// the watched-to-end edge: the playback loop clears the position at EOF,
// so a played entry with no position left has nothing to resume; an
// entry never played (play_count 0) or with an unknown duration cannot
// count, and one quit inside the 5s noise floor lands in no bucket — it
// simply shows under All.
bool matchesCategory(const LibraryEntry& e, LibraryCategory c);

// The Recent bucket's order: newest content first, path as the tiebreak
// (the store's own order is path-only).
bool compareRecent(const LibraryEntry& a, const LibraryEntry& b);

// The search box's rule (docs/todo.md 原型对齐批): a case-folded substring
// over the row's label source (the NFO title when a scan scraped one) and
// the path, which is also the label's fallback — so a file name always
// matches. The fold is ASCII-only on purpose: no locale in the predicate.
// An empty filter matches everything.
bool matchesLibraryFilter(const LibraryEntry& e, const NfoInfo* nfo,
                          const std::string& filter);

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

  // The scrape to show for a path: the <name>.nfo sidecar when one exists,
  // else the network half's cached answer, else null. The sidecar wins -
  // it is the local, user-editable truth. The pointer is stable until the
  // next scan() (or scrapeEntries()).
  const NfoInfo* nfoFor(const std::string& path) const;

  // The network half of the scrape (docs/mvp.md §3). scan() only reads the
  // offline sidecar, so the network stays out of the folder walk: scraping
  // is a deliberate, capped action the caller asks for. `limit` bounds the
  // number of source calls in one call (0 = no cap), each of which costs up
  // to the source's own timeout - the budget counts attempts, not successes,
  // so a source that cannot name the library must not turn one click into
  // entries_.size() timeouts. Entries that already carry a sidecar or a
  // cached scrape are skipped for free, so repeating the action only picks
  // up what is new. Returns the entries scraped in this call. No source
  // configured = no network at all.
  std::size_t scrapeEntries(std::size_t limit = kScrapeBudget);

  // The default scrape budget: five network attempts per call, each costing
  // up to the source's own timeout. A watched folder can hold hundreds of
  // files, so scraping it whole in one click is not on the table.
  static constexpr std::size_t kScrapeBudget = 5;

  // The source this store asks for network metadata, or null when the
  // session has no endpoint configured (the offline contract: no source, no
  // network). Non-owning - the window owns the scraper's lifetime.
  void setMetadataSource(const MetadataSource* src) { metadata_source_ = src; }
  const MetadataSource* metadataSource() const { return metadata_source_; }

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
  // nfo_ is what nfoFor() serves: the sidecar layer rebuilt by each scan()
  // with the network layer under it. scraped_ is kept separate so a rescan
  // can rebuild the sidecar half without dropping the network half (nothing
  // on disk says anything about it).
  std::map<std::string, NfoInfo> nfo_;
  std::map<std::string, NfoInfo> scraped_;
  const MetadataSource* metadata_source_ = nullptr;  // non-owning; null = offline
};

// Same state dir as recent.txt (XDG_STATE_HOME / APPDATA): <dir>/soar/
// library.txt. Overridable in tests exactly like the other stores — point
// XDG_STATE_HOME at a scratch tree.
std::string defaultLibraryPath();

}  // namespace soar::app

#endif  // SOAR_APP_UI_LIBRARY_H_
