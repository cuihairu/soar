// Unit tests for the directory-scan helpers (src/app/ui/dir_scan.*):
// media filtering, flat/recursive listing, and queue grouping. Pure
// std::filesystem over a scratch tree built in-process — no display, no
// media fixtures, no network.
//
// The listing cases assert exact paths and therefore pin the separator
// ("/" on POSIX, "\" on Windows): they run on POSIX only, where the
// walk's error-degradation arcs (EACCES holes, broken links) are also
// reproducible. The name filter and the grouping are separator-agnostic
// and run everywhere.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "ui/dir_scan.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#ifndef _WIN32
#  include <sys/stat.h>
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

#ifndef _WIN32
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

TEST_CASE("listMediaFiles keeps what it can when a subtree is unreadable") {
  // The walk uses the error_code directory_iterator overloads so an EACCES
  // hole degrades to a skip: the readable siblings must still come back.
  // Root can read anything, so the case is meaningless there.
  if (::geteuid() == 0) {
    MESSAGE("running as root; permission-denied arc not reproducible");
    return;
  }
  ScratchTree tree(scratchBase());
  const std::string locked = tree.root + "/locked";
  fs::create_directories(locked);
  ScratchTree::touch(locked + "/hidden.mp4");  // inside the unreadable dir
  REQUIRE(::chmod(locked.c_str(), 0000) == 0);

  // Flat mode descends nowhere, but its iterator still passes over
  // "locked" without reading it; recursive mode must skip it whole.
  const std::vector<std::string> flat = listMediaFiles(tree.root, false);
  CHECK(flat.size() == 2);
  const std::vector<std::string> deep = listMediaFiles(tree.root, true);
  REQUIRE(deep.size() == 5);
  for (const std::string& p : deep) {
    CHECK(p.find("hidden.mp4") == std::string::npos);
  }
  // And a listing rooted *at* the unreadable directory yields empty (the
  // iterate() error branch) rather than throwing.
  CHECK(listMediaFiles(locked, false).empty());
  CHECK(listSubdirectories(locked).empty());

  // Restore and remove: the scratch tree is shared by every case in the
  // binary (order not guaranteed), so "locked" must not leak into the
  // directory-count assertions of the other listings.
  REQUIRE(::chmod(locked.c_str(), 0755) == 0);
  fs::remove_all(locked);
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
  // multiply entries (a followed symlink loop would never terminate). The
  // subdirectory listing follows the same rule: symlinked dirs are absent.
  const std::vector<std::string> got = listMediaFiles(tree.root, true);
  REQUIRE(got.size() == 6);
  CHECK(got[2] == tree.root + "/link.mkv");  // symlinked file, still media
  const std::vector<std::string> subs = listSubdirectories(tree.root);
  CHECK(subs == std::vector<std::string>{"sub1", "sub2", "sub3"});
  // Remove the symlinks so the shared scratch tree stays honest for the
  // flat-listing assertions above (test order is not guaranteed).
  fs::remove(tree.root + "/loop");
  fs::remove(tree.root + "/selfloop");
  fs::remove(tree.root + "/link.mkv");
}

TEST_CASE("a broken file symlink with a media name is skipped") {
  // is_regular_file on the *link* follows it, so its error_code form is
  // the only thing standing between a dangling link and a phantom queue
  // entry: the walk must drop it.
  ScratchTree tree(scratchBase());
  fs::create_symlink(tree.root + "/gone.mp4", tree.root + "/dangling.m4v");
  const std::vector<std::string> got = listMediaFiles(tree.root, true);
  REQUIRE(got.size() == 5);  // the five real files, no dangling entry
  for (const std::string& p : got) {
    CHECK(p.find("dangling") == std::string::npos);
  }
  fs::remove(tree.root + "/dangling.m4v");
}

TEST_CASE("listMediaFiles flat mode stays at the top level only") {
  ScratchTree tree(scratchBase());
  // sub1/d.mp4 and sub1/deep/e.avi exist but must not surface without
  // the recursive flag — the picker's flat/recursive switch rides this.
  const std::vector<std::string> flat = listMediaFiles(tree.root, false);
  for (const std::string& p : flat) {
    CHECK(p.find("/sub1/") == std::string::npos);
    CHECK(p.find("/sub2/") == std::string::npos);
  }
}
#endif  // !_WIN32

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

TEST_CASE("groupByDirectory splits Windows-style backslash paths too") {
  // The separator scan accepts '\' so a queue built from Windows paths
  // groups as cleanly as a POSIX one (no separate code path — same find).
  const std::vector<std::string> entries = {
      R"(C:\media\a.mp4)",
      R"(C:\media\b.mp4)",
      R"(D:\shows\c.mkv)",
  };
  const std::vector<soar::app::DirGroup> groups = groupByDirectory(entries);
  REQUIRE(groups.size() == 2);
  CHECK(groups[0].directory == R"(C:\media)");
  CHECK(groups[0].indices == std::vector<std::size_t>{0, 1});
  CHECK(groups[1].directory == R"(D:\shows)");
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

TEST_CASE("groupByDirectory joins the first group after a long walk") {
  // Three groups, then a late entry that re-joins the first one: the
  // linear scan walks past every existing group before it matches, which
  // is the slowest arm of the find_if cluster.
  const std::vector<std::string> entries = {
      "/media/a.mp4",  "/alpha/b.mp4", "/beta/c.mp4",
      "/alpha/d.mp4",  "/beta/e.mp4",  "/media/f.mp4",
  };
  const std::vector<soar::app::DirGroup> groups = groupByDirectory(entries);
  REQUIRE(groups.size() == 3);
  CHECK(groups[0].directory == "/media");
  CHECK(groups[0].indices == std::vector<std::size_t>{0, 5});
  CHECK(groups[1].directory == "/alpha");
  CHECK(groups[1].indices == std::vector<std::size_t>{1, 3});
  CHECK(groups[2].directory == "/beta");
  CHECK(groups[2].indices == std::vector<std::size_t>{2, 4});
}

#ifndef _WIN32
TEST_CASE("listMediaFiles sorts entries the filesystem returned unordered") {
  // Creation order is reverse alphabetical on purpose: the output sort
  // has to do real reordering work, not just pass an already-sorted run
  // through.
  ScratchTree tree(scratchBase());
  const std::string dir = tree.root + "/unordered";
  std::error_code ec;
  fs::create_directories(dir, ec);
  REQUIRE(!ec);
  for (const char* name : {"z.mp4", "m.avi", "a.mkv", "b.mp4", "mid.mp4"}) {
    std::ofstream out(dir + "/" + name, std::ios::binary);
    out << 'x';
  }
  const std::vector<std::string> got = listMediaFiles(dir, false);
  REQUIRE(got.size() == 5);
  CHECK(got[0] == dir + "/a.mkv");
  CHECK(got[1] == dir + "/b.mp4");
  CHECK(got[2] == dir + "/m.avi");
  CHECK(got[3] == dir + "/mid.mp4");
  CHECK(got[4] == dir + "/z.mp4");
}

TEST_CASE("listSubdirectories sorts more than a handful of folders") {
  // Its own scratch root: the shared ScratchTree is per-process, and the
  // unordered-scan case above leaves its own directory behind there.
  const std::string root = scratchBase() + "/sortdirs";
  for (const char* name : {"zzz", "mmm", "aaa", "sub1", "sub2", "sub3"}) {
    std::error_code ec;
    fs::create_directories(root + "/" + name, ec);
    REQUIRE(!ec);
  }
  const std::vector<std::string> got = listSubdirectories(root);
  REQUIRE(got.size() == 6);
  CHECK(got[0] == "aaa");
  CHECK(got[1] == "mmm");
  CHECK(got[2] == "sub1");
  CHECK(got[3] == "sub2");
  CHECK(got[4] == "sub3");
  CHECK(got[5] == "zzz");
}
#endif  // !_WIN32
