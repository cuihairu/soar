#include "ui/library.h"

#include "ui/dir_scan.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <type_traits>

#ifdef _WIN32
#  include <direct.h>
#else
#  include <sys/stat.h>
#endif

namespace soar::app {

namespace {

std::string parentDir(const std::string& path) {
  const std::size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

// Creates dir and any missing parents (best effort; save() surfaces real
// failures). Mirrors ui_state.cpp's ensureDir — the two store families
// must not drift on where they are allowed to create directories.
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

// Case-folded (Windows only), separator-normalized prefix test with an
// explicit boundary: folder "C:/Movies" must not swallow "C:/Movies-x".
// The appended slash is the boundary — the folder itself is never "under"
// itself.
bool isUnderFolder(const std::string& path, const std::string& folder) {
  auto norm = [](std::string s) {
    for (char& c : s) {
      if (c == '\\') c = '/';
#ifdef _WIN32
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
#endif
    }
    return s;
  };
  std::string f = norm(folder);
  if (f.empty()) return false;
  if (f.back() != '/') f += '/';
  const std::string p = norm(path);
  return p.size() > f.size() && p.compare(0, f.size(), f) == 0;
}

// Strips the trailing CR/space run (files edited on Windows; RecentStore
// does the same to its lines) — the path is the last field, so trailing
// whitespace on the line is trailing whitespace on the path.
void trimTail(std::string& line) {
  while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
    line.pop_back();
  }
}

// Strict integer field: the whole token must parse (endptr at the end),
// which is what makes corrupt lines skippable instead of silently
// zero-filling history onto the wrong file. Negative tokens never fit an
// unsigned field — wrap-around assignment would launder them in.
template <typename T>
bool parseField(const std::string& token, T& out) {
  if (token.empty()) return false;
  errno = 0;
  char* end = nullptr;
  long long v = std::strtoll(token.c_str(), &end, 10);
  if (errno != 0 || end != token.c_str() + token.size()) return false;
  if constexpr (std::is_unsigned_v<T>) {
    if (v < 0) return false;
  }
  out = static_cast<T>(v);
  return true;
}

// Store shape (one record per line, fields space-separated, the path last
// so paths with spaces need no quoting):
//   f <folder>
//   e <size> <mtime> <pos_ms> <dur_ms> <last_played> <plays> <path>
// Unknown record types are skipped, not rejected — a future format can add
// record kinds without orphaning this reader.
bool parseEntryLine(const std::string& line, LibraryEntry& e) {
  // Six numeric fields between the type mark and the path.
  std::vector<std::string> fields;
  std::size_t pos = 2;  // after "e "
  for (int i = 0; i < 6; ++i) {
    const std::size_t sp = line.find(' ', pos);
    if (sp == std::string::npos) return false;
    fields.push_back(line.substr(pos, sp - pos));
    pos = sp + 1;
  }
  if (pos > line.size()) return false;
  const std::string path = line.substr(pos);
  if (path.empty()) return false;
  if (!parseField(fields[0], e.size) || !parseField(fields[1], e.mtime) ||
      !parseField(fields[2], e.position_ms) ||
      !parseField(fields[3], e.duration_ms) ||
      !parseField(fields[4], e.last_played) ||
      !parseField(fields[5], e.play_count)) {
    return false;
  }
  e.path = path;
  return true;
}

// Decodes the five predefined XML entities in text content. Anything else
// (numeric references, nested tags) passes through untouched — the sidecar
// is a display hint, not trusted markup. The ampersand decodes LAST:
// first-to-last would turn the literal text "&amp;lt;" into "<" (decode
// twice); last-to-first leaves it as "&lt;".
std::string decodeXmlEntities(std::string s) {
  const char* ents[] = {"lt;", "gt;", "quot;", "apos;", "amp;"};
  const char* reps[] = {"<", ">", "\"", "'", "&"};
  for (std::size_t i = 0; i < 5; ++i) {
    std::string out;
    const std::string pat = std::string("&") + ents[i];
    std::size_t pos = 0;
    while (pos < s.size()) {
      const std::size_t at = s.find(pat, pos);
      if (at == std::string::npos) {
        out.append(s, pos, std::string::npos);
        break;
      }
      out.append(s, pos, at - pos);
      out += reps[i];
      pos = at + pat.size();
    }
    s = std::move(out);
  }
  return s;
}

// First occurrence of <tag>...</tag> in the document, entity-decoded and
// whitespace-trimmed. No XML parser on purpose: Kodi-style NFO is XML-ish
// text written by tools and humans; a malformed file degrades to an empty
// field and the UI falls back to the file name.
bool extractXmlText(const std::string& doc, const char* tag, std::string& out) {
  const std::string open = std::string("<") + tag + ">";
  const std::string close = std::string("</") + tag + ">";
  const std::size_t a = doc.find(open);
  if (a == std::string::npos) return false;
  const std::size_t body = a + open.size();
  const std::size_t b = doc.find(close, body);
  if (b == std::string::npos) return false;
  std::string text = doc.substr(body, b - body);
  const std::size_t first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return false;
  text.erase(0, first);
  text.erase(text.find_last_not_of(" \t\r\n") + 1);
  out = decodeXmlEntities(std::move(text));
  return true;
}

// Reads the sidecar next to a media file (<name> with the last extension
// swapped for .nfo) and parses it. Returns false when there is no readable
// sidecar or it carries neither a title nor a plot (an all-empty scrape is
// stored as "no scrape" so the UI's null check stays the only check).
bool readNfoSidecar(const std::string& media_path, NfoInfo& out) {
  std::filesystem::path p(media_path);
  p.replace_extension(".nfo");
  std::error_code ec;
  if (!std::filesystem::is_regular_file(p, ec) || ec) return false;
  // Bound the read: a stray huge file must not turn the scan into an
  // unbounded buffer; real NFO sidecars are text-sized.
  std::ifstream in(p, std::ios::binary);
  if (!in.good()) return false;
  std::string bytes(1 << 20, '\0');
  in.read(bytes.data(), bytes.size());
  bytes.resize(static_cast<std::size_t>(in.gcount()));
  if (!extractXmlText(bytes, "title", out.title)) out.title.clear();
  if (!extractXmlText(bytes, "plot", out.plot)) out.plot.clear();
  return !out.title.empty() || !out.plot.empty();
}

}  // namespace

std::int64_t resumePositionMs(const LibraryEntry& e) {
  constexpr std::int64_t kEdgeMs = 5000;
  if (e.position_ms < kEdgeMs) return 0;
  if (e.duration_ms <= 0) return 0;  // never learned: do not guess
  if (e.position_ms > e.duration_ms - kEdgeMs) return 0;
  return e.position_ms;
}

void MediaLibrary::load() {
  folders_.clear();
  entries_.clear();
  std::ifstream in(path_);
  if (!in.good()) return;
  // Maps give the two dedupe rules for free: repeated folder lines keep
  // the first position, repeated entry paths collapse last-wins (a
  // hand-edited file may repeat; the UI must not show one source twice).
  std::map<std::string, LibraryEntry> by_path;
  std::string line;
  while (std::getline(in, line)) {
    trimTail(line);
    if (line.size() < 2 || line[1] != ' ') continue;
    if (line[0] == 'f') {
      const std::string dir = line.substr(2);
      if (dir.empty()) continue;
      if (std::find(folders_.begin(), folders_.end(), dir) == folders_.end()) {
        folders_.push_back(dir);
      }
    } else if (line[0] == 'e') {
      LibraryEntry e;
      if (parseEntryLine(line, e)) by_path[e.path] = e;
    }
    if (by_path.size() >= kMaxEntries) break;
  }
  for (auto& [path, e] : by_path) entries_.push_back(std::move(e));
}

bool MediaLibrary::save() const {
  const std::string dir = parentDir(path_);
  if (!dir.empty()) ensureDir(dir);
  const std::string tmp = path_ + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out.good()) return false;
    for (const auto& f : folders_) {
      out << "f " << f << '\n';
      if (!out.good()) return false;
    }
    for (const auto& e : entries_) {
      // std::uintmax_t prints through %zu-sized limits nowhere portable —
      // the explicit cast keeps every field in <cstdint> territory.
      out << "e " << static_cast<unsigned long long>(e.size) << ' ' << e.mtime
          << ' ' << e.position_ms << ' ' << e.duration_ms << ' ' << e.last_played
          << ' ' << e.play_count << ' ' << e.path << '\n';
      if (!out.good()) return false;
    }
  }
#ifdef _WIN32
  std::remove(path_.c_str());
#endif
  return std::rename(tmp.c_str(), path_.c_str()) == 0;
}

void MediaLibrary::addFolder(std::string dir) {
  if (std::find(folders_.begin(), folders_.end(), dir) != folders_.end()) {
    return;
  }
  folders_.push_back(std::move(dir));
}

bool MediaLibrary::removeFolder(const std::string& dir) {
  const auto it = std::find(folders_.begin(), folders_.end(), dir);
  if (it == folders_.end()) return false;
  folders_.erase(it);
  return true;
}

void MediaLibrary::scan() {
  // Move the current set into a keyed map: known files merge in O(log n)
  // per discovered path, and the survivors of the prune below iterate in
  // the same sorted order the vector keeps.
  std::map<std::string, LibraryEntry> existing;
  for (auto& e : entries_) existing.emplace(e.path, std::move(e));
  entries_.clear();
  // The scrape cache follows the same rebuild-the-whole-set rule as the
  // entries: a sidecar that vanished (or a file that left a watched
  // folder) drops out by simply not being re-collected.
  nfo_.clear();

  // Only folders that actually resolved get a say in pruning: their walk
  // is the evidence a file is gone. A missing/unreadable folder is skipped
  // whole — its entries keep their history untouched.
  std::vector<std::string> walked;
  // The merge is keyed by path so overlapping watched folders (a parent
  // and its child both watched) contribute one entry, not two.
  std::map<std::string, LibraryEntry> merged;
  for (const std::string& dir : folders_) {
    std::error_code probe;
    if (!std::filesystem::is_directory(std::filesystem::path(dir), probe) ||
        probe) {
      continue;
    }
    walked.push_back(dir);
    for (const std::string& file : listMediaFiles(dir, /*recursive=*/true)) {
      if (merged.count(file) != 0) continue;  // seen via an earlier folder
      LibraryEntry e;
      const auto prev = existing.find(file);
      if (prev != existing.end()) {
        e = std::move(prev->second);
        existing.erase(prev);
      } else {
        e.path = file;
      }
      std::error_code ec;
      const std::uintmax_t sz =
          std::filesystem::file_size(std::filesystem::path(file), ec);
      if (!ec) e.size = sz;
      const auto wt =
          std::filesystem::last_write_time(std::filesystem::path(file), ec);
      if (!ec) {
        // file_time_type's epoch is unspecified (C++17 has no clock_cast):
        // the count is an opaque change token, not a wall timestamp —
        // mtime comparisons are only ever made against values this same
        // process wrote.
        e.mtime = static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                wt.time_since_epoch())
                .count());
      }
      NfoInfo nfo;
      if (readNfoSidecar(file, nfo)) nfo_[file] = std::move(nfo);
      merged.emplace(file, std::move(e));
    }
  }
  // Direct-opened sources (URLs, files outside every walked folder)
  // survive every scan — their history is not derivable from the file
  // system, so only a watched folder's walk may take an entry away.
  for (auto& [path, e] : existing) {
    const bool watched = std::any_of(
        walked.begin(), walked.end(),
        [&](const std::string& dir) { return isUnderFolder(path, dir); });
    if (!watched) merged.emplace(path, std::move(e));
  }
  for (auto& [path, e] : merged) {
    if (entries_.size() >= kMaxEntries) break;
    entries_.push_back(std::move(e));
  }
}

const NfoInfo* MediaLibrary::nfoFor(const std::string& path) const {
  const auto it = nfo_.find(path);
  return it == nfo_.end() ? nullptr : &it->second;
}

const LibraryEntry* MediaLibrary::find(const std::string& path) const {
  const auto it = std::find_if(entries_.begin(), entries_.end(),
                               [&](const LibraryEntry& e) {
                                 return e.path == path;
                               });
  return it == entries_.end() ? nullptr : &*it;
}

LibraryEntry* MediaLibrary::upsert(const std::string& path) {
  const auto it = std::find_if(entries_.begin(), entries_.end(),
                               [&](const LibraryEntry& e) {
                                 return e.path == path;
                               });
  if (it != entries_.end()) return &*it;
  LibraryEntry e;
  e.path = path;
  // Keep the sorted-by-path invariant: insert at the first greater slot.
  const auto at = std::lower_bound(entries_.begin(), entries_.end(), path,
                                   [](const LibraryEntry& e,
                                      const std::string& key) {
                                     return e.path < key;
                                   });
  return &*entries_.insert(at, std::move(e));
}

void MediaLibrary::recordOpen(const std::string& path, std::int64_t now_epoch) {
  LibraryEntry* e = upsert(path);
  e->last_played = now_epoch;
  ++e->play_count;
}

void MediaLibrary::recordProgress(const std::string& path,
                                  std::int64_t position_ms,
                                  std::int64_t duration_ms) {
  LibraryEntry* e = upsert(path);
  e->position_ms = position_ms;
  e->duration_ms = duration_ms;
}

void MediaLibrary::clearPosition(const std::string& path) {
  const auto it = std::find_if(entries_.begin(), entries_.end(),
                               [&](const LibraryEntry& e) {
                                 return e.path == path;
                               });
  if (it != entries_.end()) it->position_ms = 0;
}

bool MediaLibrary::removeEntry(const std::string& path) {
  const auto it = std::find_if(entries_.begin(), entries_.end(),
                               [&](const LibraryEntry& e) {
                                 return e.path == path;
                               });
  if (it == entries_.end()) return false;
  entries_.erase(it);
  return true;
}

std::string defaultLibraryPath() {
  const char* dir = nullptr;
#ifdef _WIN32
  dir = std::getenv("APPDATA");
  if (dir && *dir) return std::string(dir) + "\\soar\\library.txt";
  return "soar-library.txt";
#else
  dir = std::getenv("XDG_STATE_HOME");
  if (dir && *dir) return std::string(dir) + "/soar/library.txt";
  const char* home = std::getenv("HOME");
  if (home && *home) {
    return std::string(home) + "/.local/state/soar/library.txt";
  }
  return "soar-library.txt";
#endif
}

}  // namespace soar::app
