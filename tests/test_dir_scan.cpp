// Unit tests for the directory-scan helpers (src/app/ui/dir_scan.*):
// media filtering, flat/recursive listing, and queue grouping. Pure
// std::filesystem over a scratch tree built in-process — no display, no
// media fixtures, no network.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "ui/dir_scan.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <unistd.h>
#endif

namespace fs = std::filesystem;
using soar::app::groupByDirectory;
using soar::app::isMediaFile;
using soar::app::listMediaFiles;
using soar::app::listSubdirectories;

namespace {

// One scratch tree per process, built once: two media files and a decoy at
// the root, three subdirectories of which one nests deeper and one holds
// only non-media. Everything is created empty — the scan filters by name,
// it never opens content.
struct ScratchTree {
  std::string root;

  explicit ScratchTree(const std::string& base) : root(base + "/tree") {
    fs::create_directories(root + "/sub1/deep");
    fs::create_directories(root + "/sub2");
    fs::create_directories(root + "/sub3");
    touch(root + "/a.mp4");
    touch(root + "/b.MKV");   // extension case must not matter
    touch(root + "/c.txt");   // non-media decoy at the root
    touch(root + "/.mp4");    // dotfile stem: not an extension
    touch(root + "/sub1/d.mp4");
    touch(root + "/sub1/deep/e.avi");
    touch(root + "/sub2/f.mkv");
    touch(root + "/sub3/g.txt");  // a folder with nothing playable
  }

  static void touch(const std::string& path) {
    std::ofstream out(path, std::ios::binary);
    out << 'x';
  }
};

const std::string& scratchBase() {
  static const std::string base = [] {
    // mkdtemp is POSIX-only (MSVC has none): the standard temp directory
    // plus the pid keeps concurrent test processes out of each other's
    // trees just as well.
    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec);
    if (ec) dir = fs::path("/tmp");
#ifdef _WIN32
    dir /= "soar_dir_scan_" + std::to_string(::GetCurrentProcessId());
#else
    dir /= "soar_dir_scan_" + std::to_string(::getpid());
#endif
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir.string();
  }();
  return base;
}

}  // namespace

TEST_CASE("isMediaFile matches container extensions case-insensitively") {
  CHECK(isMediaFile("a.mp4"));
  CHECK(isMediaFile("B.MKV"));
  CHECK(isMediaFile("show.s01e02.webm"));
  CHECK_FALSE(isMediaFile("notes.txt"));
  CHECK_FALSE(isMediaFile("noext"));
  CHECK_FALSE(isMediaFile(".mp4"));    // dotfile, no stem before the dot
  CHECK_FALSE(isMediaFile("archive.mp4.bak"));  // extension is the last one
  CHECK_FALSE(isMediaFile("f.torrent"));  // download source, not media
}

TEST_CASE("listMediaFiles flat mode returns only the top level, sorted") {
  ScratchTree tree(scratchBase());
  const std::vector<std::string> got = listMediaFiles(tree.root, false);
  REQUIRE(got.size() == 2);
  // Byte order: the full paths share the prefix, so names decide.
  CHECK(got[0] == tree.root + "/a.mp4");
  CHECK(got[1] == tree.root + "/b.MKV");
}

TEST_CASE("listMediaFiles recursive descends into nested directories") {
  ScratchTree tree(scratchBase());
  const std::vector<std::string> got = listMediaFiles(tree.root, true);
  REQUIRE(got.size() == 5);
  CHECK(got[0] == tree.root + "/a.mp4");
  CHECK(got[1] == tree.root + "/b.MKV");
  CHECK(got[2] == tree.root + "/sub1/d.mp4");
  CHECK(got[3] == tree.root + "/sub1/deep/e.avi");
  CHECK(got[4] == tree.root + "/sub2/f.mkv");
}

TEST_CASE("listMediaFiles on a missing or empty directory yields nothing") {
  CHECK(listMediaFiles(scratchBase() + "/does-not-exist", true).empty());
  ScratchTree tree(scratchBase());
  // sub3 holds only c.txt-style decoys: present, but no media in it.
  CHECK(listMediaFiles(tree.root + "/sub3", false).empty());
}

TEST_CASE("listSubdirectories returns immediate real directories sorted") {
  ScratchTree tree(scratchBase());
  const std::vector<std::string> got = listSubdirectories(tree.root);
  REQUIRE(got.size() == 3);
  CHECK(got[0] == "sub1");
  CHECK(got[1] == "sub2");
  CHECK(got[2] == "sub3");
  CHECK(listSubdirectories(tree.root + "/does-not-exist").empty());
}

TEST_CASE("directory symlinks are not followed, file symlinks count") {
  ScratchTree tree(scratchBase());
  fs::create_directory_symlink(tree.root + "/sub2", tree.root + "/loop");
  std::error_code ec;
  fs::create_directory_symlink(tree.root, tree.root + "/selfloop", ec);
  REQUIRE(ec == std::error_code());  // platform must support dir symlinks
  fs::create_symlink(tree.root + "/b.MKV", tree.root + "/link.mkv");

  // The walk visits sub1..sub3 once each; "loop" and "selfloop" must not
  // multiply entries (a followed symlink loop would never terminate).
  const std::vector<std::string> got = listMediaFiles(tree.root, true);
  REQUIRE(got.size() == 6);
  CHECK(got[2] == tree.root + "/link.mkv");  // symlinked file, still media
  // Remove the symlinks so the shared scratch tree stays honest for the
  // flat-listing assertions above (test order is not guaranteed).
  fs::remove(tree.root + "/loop");
  fs::remove(tree.root + "/selfloop");
  fs::remove(tree.root + "/link.mkv");
}

TEST_CASE("groupByDirectory keeps queue order and ascending indices") {
  const std::vector<std::string> entries = {
      "/media/a.mp4",       // first sight of /media
      "/media/b.mp4",
      "/other/c.mkv",       // first sight of /other
      "/media/d.mp4",       // /media again — joins the existing group
  };
  const std::vector<soar::app::DirGroup> groups = groupByDirectory(entries);
  REQUIRE(groups.size() == 2);
  CHECK(groups[0].directory == "/media");
  CHECK(groups[0].indices == std::vector<std::size_t>{0, 1, 3});
  CHECK(groups[1].directory == "/other");
  CHECK(groups[1].indices == std::vector<std::size_t>{2});
}

TEST_CASE("groupByDirectory parks URLs and bare names in the empty group") {
  const std::vector<std::string> entries = {
      "http://example.com/stream.m3u8",
      "solo.mp4",           // no separator: not a directory member either
      "/media/a.mp4",
  };
  const std::vector<soar::app::DirGroup> groups = groupByDirectory(entries);
  REQUIRE(groups.size() == 2);
  CHECK(groups[0].directory.empty());
  CHECK(groups[0].indices == std::vector<std::size_t>{0, 1});
  CHECK(groups[1].directory == "/media");
  CHECK(groups[1].indices == std::vector<std::size_t>{2});
}

TEST_CASE("groupByDirectory of an empty queue is empty") {
  CHECK(groupByDirectory({}).empty());
}
