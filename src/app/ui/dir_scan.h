// Directory scanning for the "open folder" import and the playlist
// folder-tree view (docs/ui-design.md §2): media-file filtering, flat and
// recursive listing, and the parent-directory grouping that turns the flat
// play queue into a tree. Pure std::filesystem over UTF-8 paths — no SDL
// or ImGui here on purpose, same contract as ui_state.h: the window layer
// owns rendering and input, this stays unit-testable without a display.

#ifndef SOAR_APP_UI_DIR_SCAN_H_
#define SOAR_APP_UI_DIR_SCAN_H_

#include <cstddef>
#include <string>
#include <vector>

namespace soar::app {

// Whether `name` (a file name, not a path) carries a media container
// extension the backend plays directly. The set mirrors the Windows
// open-file filter minus .torrent/.m3u8/.mpd: a folder import queues
// playable media, not download sources or remote-playlist pointers.
// Case-insensitive; dotfiles like ".mp4" (no stem) do not match.
bool isMediaFile(const std::string& name);

// Media files in `dir`, sorted by full path (byte order — deterministic on
// every platform). With `recursive`, subdirectories are descended
// depth-first; directory symlinks are never followed (no cycles, no
// escapes out of the picked tree) while symlinked *files* still count.
// A missing or unreadable directory yields an empty list, not an error —
// the caller reports "0 imported" through the same path.
std::vector<std::string> listMediaFiles(const std::string& dir, bool recursive);

// Immediate subdirectories of `dir`, sorted by name. "." never appears
// (the caller renders ".." itself). Empty for a missing/unreadable dir.
std::vector<std::string> listSubdirectories(const std::string& dir);

// One folder node of the playlist tree: a parent directory plus the queue
// indices that live under it, ascending. Entries that are not local paths
// (URLs, scheme sources, bare names) group under directory "" — the
// window renders that as the flat "Other sources" section.
struct DirGroup {
  std::string directory;
  std::vector<std::size_t> indices;
};

// Groups queue `entries` by parent directory. Group order = first
// occurrence of each directory in the queue (the tree grows in queue
// order, not alphabetical); indices ascend inside each group. A local
// path is anything with a path separator and no scheme; everything else
// (http://, rtsp://, bare names) lands in the single "" group.
std::vector<DirGroup> groupByDirectory(const std::vector<std::string>& entries);

}  // namespace soar::app

#endif  // SOAR_APP_UI_DIR_SCAN_H_
