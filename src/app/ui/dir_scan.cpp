#include "ui/dir_scan.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

namespace soar::app {
namespace {

namespace fs = std::filesystem;

// Same containers as the Windows open-file dialog filter (player_window.cpp),
// with .torrent/.m3u8/.mpd left out: folder import is "queue what plays",
// and torrent/remote-playlist sources have their own entry points.
constexpr const char* kMediaExtensions[] = {
    "mp4", "m4v", "mkv", "webm", "avi", "mov",  "flv",  "wmv",
    "ts",  "m2ts", "mp3", "m4a", "aac", "flac", "wav",  "ogg", "opus",
};

std::string lower(const std::string& s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

// Directory iterators must not throw on EACCES holes inside the tree: a
// folder import that hits one unreadable subdirectory should still bring
// the rest in. The error_code overloads degrade to end() instead.
fs::directory_iterator iterate(const fs::path& dir, std::error_code& ec) {
  return fs::directory_iterator(dir, fs::directory_options::skip_permission_denied,
                                ec);
}

}  // namespace

bool isMediaFile(const std::string& name) {
  const std::size_t dot = name.find_last_of('.');
  // A leading dot with nothing before it is a dotfile stem (".mp4"), not
  // an extension — only match when at least one character precedes.
  if (dot == std::string::npos || dot == 0) return false;
  const std::string ext = lower(name.substr(dot + 1));
  for (const char* known : kMediaExtensions) {
    if (ext == known) return true;
  }
  return false;
}

std::vector<std::string> listMediaFiles(const std::string& dir, bool recursive) {
  std::vector<std::string> out;
  std::error_code ec;
  fs::directory_iterator it = iterate(fs::path(dir), ec);
  if (ec) return out;

  // Explicit stack rather than recursive_directory_iterator: the iterator
  // variant has a follow_directory_symlink option but no "skip symlinked
  // directories" one, and the contract here is exactly that.
  std::vector<fs::path> pending{fs::path(dir)};
  while (!pending.empty()) {
    const fs::path current = pending.back();
    pending.pop_back();
    it = iterate(current, ec);
    if (ec) continue;
    for (const fs::directory_entry& entry : it) {
      ec.clear();
      // symlink_status = the entry itself, no following: only real
      // directories go onto the descent stack, so a symlink loop in the
      // tree cannot spin this walk.
      if (entry.is_symlink()) {
        // A symlinked *file* still plays; resolve it through is_regular_file
        // (the error_code form — unreadable links degrade to a skip).
        std::error_code file_ec;
        if (entry.is_regular_file(file_ec) && !file_ec &&
            isMediaFile(entry.path().filename().string())) {
          out.push_back(entry.path().string());
        }
        continue;
      }
      std::error_code kind_ec;
      if (entry.is_directory(kind_ec) && !kind_ec) {
        if (recursive) pending.push_back(entry.path());
        continue;
      }
      if (entry.is_regular_file(kind_ec) && !kind_ec &&
          isMediaFile(entry.path().filename().string())) {
        out.push_back(entry.path().string());
      }
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::string> listSubdirectories(const std::string& dir) {
  std::vector<std::string> out;
  std::error_code ec;
  fs::directory_iterator it = iterate(fs::path(dir), ec);
  if (ec) return out;
  for (const fs::directory_entry& entry : it) {
    std::error_code kind_ec;
    // Real directories only (same no-symlink rule as the walk above).
    if (!entry.is_symlink() && entry.is_directory(kind_ec) && !kind_ec) {
      out.push_back(entry.path().filename().string());
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<DirGroup> groupByDirectory(const std::vector<std::string>& entries) {
  std::vector<DirGroup> groups;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const std::string& uri = entries[i];
    std::string parent;
    const bool has_scheme = uri.find("://") != std::string::npos;
    if (!has_scheme) {
      const std::size_t slash = uri.find_last_of("/\\");
      if (slash != std::string::npos) parent = uri.substr(0, slash);
    }
    // Linear scan: queues are tens of entries, and first-occurrence order
    // falls out of the append-if-missing without an extra index map.
    auto it = std::find_if(groups.begin(), groups.end(),
                           [&](const DirGroup& g) { return g.directory == parent; });
    if (it == groups.end()) {
      groups.push_back(DirGroup{parent, {}});
      groups.back().indices.push_back(i);
    } else {
      it->indices.push_back(i);
    }
  }
  return groups;
}

}  // namespace soar::app
