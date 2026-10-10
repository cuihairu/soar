// Unit tests for the media library store (src/app/ui/library.*): watched
// folders, the scan merge/prune semantics, the playback history API, and
// the persisted line format. Pure std::filesystem over a scratch tree
// built in-process — no display, no media fixtures, no network. Every
// store file lands under a per-process scratch dir (soar_library_*).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "ui/library.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#ifndef _WIN32
#  include <unistd.h>
#endif

namespace fs = std::filesystem;
using soar::app::defaultLibraryPath;
using soar::app::LibraryEntry;
using soar::app::MediaLibrary;
using soar::app::NfoInfo;
using soar::app::resumePositionMs;

namespace {

std::string makeTempDir() {
  std::error_code ec;
  fs::path dir = fs::temp_directory_path(ec);
  if (ec) dir = fs::path("/tmp");
  // One fresh directory per call: a pid plus a call counter keeps the
  // per-case trees out of each other without a removal race.
  static int calls = 0;
  dir /= "soar_library_" + std::to_string(::getpid()) + "_" +
         std::to_string(++calls);
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  return dir.string();
}

void touch(const std::string& path, const std::string& content = "x") {
  std::ofstream out(path, std::ios::binary);
  out << content;
}

std::string slurp(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

// A watched tree: two media files at the root, one nested, one nested
// deeper, and two decoys the extension filter must skip.
struct Tree {
  std::string root;

  explicit Tree(const std::string& base) : root(base + "/films") {
    fs::create_directories(root + "/season1/extras");
    touch(root + "/a.mp4");
    touch(root + "/b.mkv", "bigger file");
    touch(root + "/notes.txt");
    touch(root + "/season1/c.mp4");
    touch(root + "/season1/extras/d.avi");
    touch(root + "/season1/extras/cover.jpg");
  }
};

}  // namespace

TEST_CASE("empty store loads as an empty library") {
  MediaLibrary lib(makeTempDir() + "/library.txt");
  lib.load();
  CHECK(lib.folders().empty());
  CHECK(lib.entries().empty());
}

TEST_CASE("save and load round-trips folders and entries") {
  const std::string dir = makeTempDir();
  const std::string store = dir + "/library.txt";
  {
    MediaLibrary lib(store);
    lib.addFolder(dir + "/films");
    lib.addFolder("/watches/Movies, tonight");  // spaces in the folder path
    lib.recordOpen("http://example.com/a video.mp4", 1700000000);
    lib.recordProgress("http://example.com/a video.mp4", 65000, 600000);
    lib.recordOpen("/watches/local file.mp4", 1700000100);
    CHECK(lib.save());
  }
  MediaLibrary again(store);
  again.load();
  REQUIRE(again.folders().size() == 2);
  CHECK(again.folders()[0] == dir + "/films");
  CHECK(again.folders()[1] == "/watches/Movies, tonight");
  REQUIRE(again.entries().size() == 2);
  // Entries come back sorted by path (byte order), not insertion order.
  CHECK(again.entries()[0].path == "/watches/local file.mp4");
  CHECK(again.entries()[1].path == "http://example.com/a video.mp4");
  const LibraryEntry& url = again.entries()[1];
  CHECK(url.position_ms == 65000);
  CHECK(url.duration_ms == 600000);
  CHECK(url.last_played == 1700000000);
  CHECK(url.play_count == 1);
  CHECK(again.entries()[0].play_count == 1);
}

TEST_CASE("corrupt lines are skipped and duplicates collapse") {
  const std::string dir = makeTempDir();
  const std::string store = dir + "/library.txt";
  touch(store,
        "f /one\n"
        "f /one\n"                     // repeated folder: first position wins
        "f\n"                          // empty folder: skipped
        "x nonsense record type\n"     // unknown type: skipped
        "e 1 2 3\n"                    // truncated field list: skipped
        "e a b c d e f /not-num.mp4\n" // non-numeric fields: skipped
        "e 10 20 0 0 0 7 /dupe.mp4\n"
        "e 11 21 30000 90000 555 9 /dupe.mp4\n" // last wins
        "e 10 20 0 0 0 1 /ok.mp4\r\n"  // CRLF tolerated
        "e 10 20 0 0 0 1\n"            // no path: skipped
  );
  MediaLibrary lib(store);
  lib.load();
  REQUIRE(lib.folders().size() == 1);
  CHECK(lib.folders()[0] == "/one");
  REQUIRE(lib.entries().size() == 2);
  CHECK(lib.entries()[0].path == "/dupe.mp4");
  CHECK(lib.entries()[0].size == 11);
  CHECK(lib.entries()[0].position_ms == 30000);
  CHECK(lib.entries()[0].play_count == 9);
  CHECK(lib.entries()[1].path == "/ok.mp4");
  CHECK(lib.entries()[1].play_count == 1);
}

TEST_CASE("folder add is idempotent and remove reports reality") {
  MediaLibrary lib(makeTempDir() + "/library.txt");
  lib.addFolder("/a");
  lib.addFolder("/a");
  lib.addFolder("/b");
  CHECK(lib.folders().size() == 2);
  CHECK(lib.removeFolder("/a"));
  CHECK_FALSE(lib.removeFolder("/a"));
  CHECK_FALSE(lib.removeFolder("/never"));
  REQUIRE(lib.folders().size() == 1);
  CHECK(lib.folders()[0] == "/b");
}

TEST_CASE("scan discovers media recursively and stats each file") {
  const std::string dir = makeTempDir();
  Tree tree(dir);
  MediaLibrary lib(dir + "/library.txt");
  lib.addFolder(tree.root);
  lib.scan();
  REQUIRE(lib.entries().size() == 4);
  // Byte-order by path: root files first, then the nested ones.
  CHECK(lib.entries()[0].path == tree.root + "/a.mp4");
  CHECK(lib.entries()[1].path == tree.root + "/b.mkv");
  CHECK(lib.entries()[2].path == tree.root + "/season1/c.mp4");
  CHECK(lib.entries()[3].path == tree.root + "/season1/extras/d.avi");
  for (const auto& e : lib.entries()) {
    CHECK(e.size > 0);
    CHECK(e.mtime != 0);
    CHECK(e.position_ms == 0);
    CHECK(e.play_count == 0);
  }
  // The b file got real content: its stat must say so, not the 'x' default.
  CHECK(lib.entries()[1].size == 11);
}

TEST_CASE("scan keeps history fields and refreshes stat fields") {
  const std::string dir = makeTempDir();
  Tree tree(dir);
  MediaLibrary lib(dir + "/library.txt");
  lib.addFolder(tree.root);
  lib.scan();
  lib.recordOpen(tree.root + "/a.mp4", 1700000000);
  lib.recordProgress(tree.root + "/a.mp4", 120000, 300000);
  lib.recordOpen(tree.root + "/season1/c.mp4", 1700000005);
  CHECK(lib.save());

  // Grow a watched file, then rescan: the position survives, the size
  // tracks the file system.
  touch(tree.root + "/a.mp4", "much bigger content now");
  lib.scan();
  const LibraryEntry* a = lib.find(tree.root + "/a.mp4");
  REQUIRE(a != nullptr);
  CHECK(a->position_ms == 120000);
  CHECK(a->duration_ms == 300000);
  CHECK(a->last_played == 1700000000);
  CHECK(a->play_count == 1);
  CHECK(a->size == 23);
}

TEST_CASE("scan prunes files deleted from a readable folder") {
  const std::string dir = makeTempDir();
  Tree tree(dir);
  MediaLibrary lib(dir + "/library.txt");
  lib.addFolder(tree.root);
  lib.scan();
  REQUIRE(lib.entries().size() == 4);
  std::error_code ec;
  fs::remove(tree.root + "/b.mkv", ec);
  lib.scan();
  REQUIRE(lib.entries().size() == 3);
  CHECK(lib.find(tree.root + "/b.mkv") == nullptr);
}

TEST_CASE("scan leaves a missing watched folder untouched") {
  const std::string dir = makeTempDir();
  Tree tree(dir);
  MediaLibrary lib(dir + "/library.txt");
  lib.addFolder(tree.root);
  lib.scan();
  REQUIRE(lib.entries().size() == 4);

  // The unmounted-drive case: the folder vanishes whole. A walk that
  // returns nothing here would mean "mass deletion" — the scan must
  // treat an unreadable folder as no evidence at all.
  std::error_code ec;
  fs::remove_all(tree.root, ec);
  lib.scan();
  CHECK(lib.entries().size() == 4);
}

TEST_CASE("scan never prunes entries outside every watched folder") {
  const std::string dir = makeTempDir();
  Tree tree(dir);
  MediaLibrary lib(dir + "/library.txt");
  lib.addFolder(tree.root);
  lib.scan();
  lib.recordOpen("http://stream.example/live.m3u8", 1700000000);
  lib.recordProgress("http://stream.example/live.m3u8", 9000, 0);
  lib.recordOpen("/elsewhere/file.mp4", 1700000001);

  // A watched folder disappearing must not take the URL with it, and a
  // rescan with the folder intact keeps the outside path too.
  lib.scan();
  CHECK(lib.find("http://stream.example/live.m3u8") != nullptr);
  CHECK(lib.find("/elsewhere/file.mp4") != nullptr);
  REQUIRE(lib.entries().size() == 6);
}

TEST_CASE("overlapping watched folders produce one entry per file") {
  const std::string dir = makeTempDir();
  Tree tree(dir);
  MediaLibrary lib(dir + "/library.txt");
  lib.addFolder(tree.root);
  lib.addFolder(tree.root + "/season1");  // nested inside the first
  lib.scan();
  REQUIRE(lib.entries().size() == 4);
  CHECK(lib.entries()[0].path == tree.root + "/a.mp4");
}

TEST_CASE("history API creates, counts, clears and forgets") {
  const std::string dir = makeTempDir();
  MediaLibrary lib(dir + "/library.txt");

  lib.recordOpen("/b.mp4", 1000);
  lib.recordOpen("/a.mp4", 1001);
  lib.recordOpen("/a.mp4", 2000);
  REQUIRE(lib.entries().size() == 2);
  // Upsert keeps the path-sorted invariant however the opens arrive.
  CHECK(lib.entries()[0].path == "/a.mp4");
  CHECK(lib.entries()[0].play_count == 2);
  CHECK(lib.entries()[0].last_played == 2000);
  CHECK(lib.entries()[1].path == "/b.mp4");
  CHECK(lib.entries()[1].play_count == 1);

  // A progress write for an unopened path still creates the row.
  lib.recordProgress("/c.mp4", 40000, 100000);
  REQUIRE(lib.entries().size() == 3);
  CHECK(lib.entries()[2].path == "/c.mp4");

  lib.clearPosition("/c.mp4");
  CHECK(lib.find("/c.mp4")->position_ms == 0);
  lib.clearPosition("/ghost.mp4");  // unknown path: no-op
  CHECK(lib.removeEntry("/a.mp4"));
  CHECK_FALSE(lib.removeEntry("/a.mp4"));
  CHECK(lib.entries().size() == 2);
}

TEST_CASE("resume policy refuses noise, unknown duration and the tail") {
  LibraryEntry e;
  e.path = "/x.mp4";

  CHECK(resumePositionMs(e) == 0);  // never played
  e.position_ms = 4999;
  CHECK(resumePositionMs(e) == 0);  // intro-scale noise
  e.position_ms = 5000;
  CHECK(resumePositionMs(e) == 0);  // duration unknown: do not guess
  e.duration_ms = 100000;
  CHECK(resumePositionMs(e) == 5000);
  e.position_ms = 95001;
  CHECK(resumePositionMs(e) == 0);  // effectively finished
  e.position_ms = 95000;
  CHECK(resumePositionMs(e) == 95000);
}

TEST_CASE("the store file is bounded at kMaxEntries") {
  const std::string dir = makeTempDir();
  const std::string store = dir + "/library.txt";
  {
    MediaLibrary lib(store);
    for (int i = 0; i < 2100; ++i) {
      lib.recordOpen("/f" + std::to_string(i) + ".mp4", i);
    }
    CHECK(lib.entries().size() == 2100);  // unbounded in memory
    CHECK(lib.save());
  }
  MediaLibrary lib(store);
  lib.load();
  CHECK(lib.entries().size() == MediaLibrary::kMaxEntries);

  // The scan cap: a watched tree larger than the cap keeps the lexically
  // first files.
  fs::create_directories(dir + "/big");
  for (int i = 0; i < 2100; ++i) {
    touch(dir + "/big/f" + std::to_string(i) + ".mp4");
  }
  lib.addFolder(dir + "/big");
  lib.scan();
  CHECK(lib.entries().size() == MediaLibrary::kMaxEntries);
}

TEST_CASE("defaultLibraryPath follows the XDG ladder") {
  // Same ladder as defaultRecentPath, one file name over: assert the
  // shared prefix contract, not a hardcoded home path.
  const std::string p = defaultLibraryPath();
  CHECK_FALSE(p.empty());
  CHECK(p.find("library.txt") != std::string::npos);
}

TEST_CASE("save into a missing directory creates it") {
  const std::string dir = makeTempDir();
  MediaLibrary lib(dir + "/deep/nested/library.txt");
  lib.recordOpen("/a.mp4", 5);
  CHECK(lib.save());
  MediaLibrary again(dir + "/deep/nested/library.txt");
  again.load();
  REQUIRE(again.entries().size() == 1);
  CHECK(again.entries()[0].path == "/a.mp4");
}

TEST_CASE("scan scrapes an NFO sidecar title and plot") {
  const std::string dir = makeTempDir();
  const std::string films = dir + "/films";
  fs::create_directories(films + "/season1");
  touch(films + "/a.mkv");
  touch(films + "/season1/b.mkv");
  {
    std::ofstream(films + "/a.nfo", std::ios::binary)
        << "<movie><title>Deep Sea</title><plot>One diver down.</plot></movie>\n";
  }
  MediaLibrary lib(dir + "/library.txt");
  lib.addFolder(films);
  lib.scan();
  // The sidecar is metadata, not media: only the two .mkv files are
  // entries, and only the one with a readable sidecar has a scrape.
  REQUIRE(lib.entries().size() == 2);
  const NfoInfo* a = lib.nfoFor(films + "/a.mkv");
  REQUIRE(a != nullptr);
  CHECK(a->title == "Deep Sea");
  CHECK(a->plot == "One diver down.");
  CHECK(lib.nfoFor(films + "/season1/b.mkv") == nullptr);
  // The scrape is re-derived, never persisted: a reload must not bring it
  // back (the sidecar file on disk is the source of truth).
  MediaLibrary again(dir + "/library.txt");
  again.load();
  CHECK(again.nfoFor(films + "/a.mkv") == nullptr);
}

TEST_CASE("sidecar text is entity-decoded, trimmed, and survives gaps") {
  const std::string dir = makeTempDir();
  const std::string films = dir + "/films";
  fs::create_directories(films);
  touch(films + "/a.mkv");
  touch(films + "/b.mkv");
  {
    std::ofstream(films + "/a.nfo", std::ios::binary)
        << "<movie>\n  <title>  A &amp; B &lt;C&gt;  </title>\n"
        << "<year>2021</year>\n</movie>\n";
  }
  {
    std::ofstream(films + "/b.nfo", std::ios::binary)
        << "<movie><title>unclosed tag</movie>\n";
  }
  MediaLibrary lib(dir + "/library.txt");
  lib.addFolder(films);
  lib.scan();
  const NfoInfo* a = lib.nfoFor(films + "/a.mkv");
  REQUIRE(a != nullptr);
  CHECK(a->title == "A & B <C>");
  // amp decodes LAST (otherwise &amp;lt; would decode twice into <).
  // The b sidecar's <title> has no closing tag: no title, and the
  // parse-only <movie> body carries neither title nor plot — no scrape.
  CHECK(lib.nfoFor(films + "/b.mkv") == nullptr);
}

TEST_CASE("a rescan follows sidecar edits and removals") {
  const std::string dir = makeTempDir();
  const std::string films = dir + "/films";
  fs::create_directories(films);
  touch(films + "/a.mkv");
  {
    std::ofstream(films + "/a.nfo", std::ios::binary)
        << "<movie><title>First Title</title></movie>\n";
  }
  MediaLibrary lib(dir + "/library.txt");
  lib.addFolder(films);
  lib.scan();
  REQUIRE(lib.nfoFor(films + "/a.mkv") != nullptr);
  CHECK(lib.nfoFor(films + "/a.mkv")->title == "First Title");
  {
    std::ofstream(films + "/a.nfo", std::ios::binary)
        << "<movie><title>Second Title</title><plot>Plot v2.</plot></movie>\n";
  }
  lib.scan();
  REQUIRE(lib.nfoFor(films + "/a.mkv") != nullptr);
  CHECK(lib.nfoFor(films + "/a.mkv")->title == "Second Title");
  CHECK(lib.nfoFor(films + "/a.mkv")->plot == "Plot v2.");
  fs::remove(films + "/a.nfo");
  lib.scan();
  CHECK(lib.nfoFor(films + "/a.mkv") == nullptr);
}

TEST_CASE("entries outside every watched folder get no scrape") {
  const std::string dir = makeTempDir();
  const std::string films = dir + "/films";
  fs::create_directories(films);
  touch(films + "/a.mkv");
  MediaLibrary lib(dir + "/library.txt");
  // Opened directly (no sidecar next to it anywhere), then scan a folder
  // that does not contain it: the entry survives the scan but carries no
  // scrape — a direct open never walks the file system for an NFO.
  lib.recordOpen("/nowhere/b.mkv", 42);
  lib.addFolder(films);
  lib.scan();
  REQUIRE(lib.entries().size() == 2);
  CHECK(lib.nfoFor("/nowhere/b.mkv") == nullptr);
}

TEST_CASE("each numeric field's failure shape is rejected on its own") {
  const std::string dir = makeTempDir();
  const std::string store = dir + "/library.txt";
  // One row per distinct parse failure, so every field of the parseField
  // chain is exercised independently: an empty token (the double space
  // yields a zero-length field), an out-of-range integer (ERANGE), a
  // token with trailing garbage (endptr not at the end), and a negative
  // value landing in an unsigned field.
  touch(store,
        "e  1 2 3 4 5 6 /empty-token.mp4\n"     // empty field[0]
        "e 99999999999999999999999999 0 0 0 0 0 /overflow.mp4\n"  // ERANGE
        "e 1x 0 0 0 0 0 /suffix.mp4\n"          // endptr not at the end
        "e -1 0 0 0 0 0 /neg-size.mp4\n"        // negative into uintmax_t
        "e 1 x 0 0 0 0 /f2.mp4\n"               // field[1] fails
        "e 1 2 x 0 0 0 /f3.mp4\n"               // field[2] fails
        "e 1 2 3 x 0 0 /f4.mp4\n"               // field[3] fails
        "e 1 2 3 4 x 0 /f5.mp4\n"               // field[4] fails
        "e 1 2 3 4 5 x /f6.mp4\n"               // field[5] fails
        "ff /bad-separator.mp4\n"               // size>=2 but no space at [1]
        "e 1 2 3 4 5 6 /kept.mp4\n");           // the only valid row
  MediaLibrary lib(store);
  lib.load();
  REQUIRE(lib.entries().size() == 1);
  CHECK(lib.entries()[0].path == "/kept.mp4");
  CHECK(lib.entries()[0].size == 1);
  CHECK(lib.entries()[0].play_count == 6);
}

TEST_CASE("a whitespace-only NFO tag body is dropped, the plot survives") {
  const std::string dir = makeTempDir();
  const std::string films = dir + "/films";
  fs::create_directories(films);
  touch(films + "/a.mkv");
  // A title of only blanks parses to nothing: the field is dropped while
  // the plot still scrapes, so the scrape is not lost with it.
  {
    std::ofstream(films + "/a.nfo", std::ios::binary)
        << "<movie><title>   </title><plot>Real plot.</plot></movie>\n";
  }
  MediaLibrary lib(dir + "/library.txt");
  lib.addFolder(films);
  lib.scan();
  const NfoInfo* a = lib.nfoFor(films + "/a.mkv");
  REQUIRE(a != nullptr);
  CHECK(a->title.empty());
  CHECK(a->plot == "Real plot.");
}

#ifndef _WIN32
TEST_CASE("defaultLibraryPath walks the env ladder to the bare fallback") {
  const char* xdg = std::getenv("XDG_STATE_HOME");
  const char* home = std::getenv("HOME");
  const std::string xdg_save = xdg ? xdg : "";
  const std::string home_save = home ? home : "";
  const bool xdg_was_set = xdg != nullptr;
  const bool home_was_set = home != nullptr;

  // An explicit, non-empty XDG_STATE_HOME wins the ladder.
  ::setenv("XDG_STATE_HOME", "/tmp/soar_state_xdg", 1);
  ::setenv("HOME", "/tmp/soar_home_saved", 1);
  CHECK(defaultLibraryPath() == "/tmp/soar_state_xdg/soar/library.txt");

  // A set-but-empty XDG_STATE_HOME counts as unset: fall through to HOME.
  ::setenv("XDG_STATE_HOME", "", 1);
  CHECK(defaultLibraryPath() ==
        "/tmp/soar_home_saved/.local/state/soar/library.txt");

  // HOME set but empty: no usable prefix, so the bare relative name is the
  // only answer left.
  ::unsetenv("XDG_STATE_HOME");
  ::setenv("HOME", "", 1);
  CHECK(defaultLibraryPath() == "soar-library.txt");

  // Both absent: same bare fallback, reached through the null arm.
  ::unsetenv("HOME");
  CHECK(defaultLibraryPath() == "soar-library.txt");

  // Restore for any test that runs after this one.
  if (xdg_was_set) ::setenv("XDG_STATE_HOME", xdg_save.c_str(), 1);
  else ::unsetenv("XDG_STATE_HOME");
  if (home_was_set) ::setenv("HOME", home_save.c_str(), 1);
  else ::unsetenv("HOME");
}
#endif
