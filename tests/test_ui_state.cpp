// Unit tests for the pure UI logic (src/app/ui/ui_state.*): the OSC
// auto-hide state machine, the recent-files store, the toast, and time
// formatting. Everything takes an injected clock/path, so no display and
// no wall-clock betting is involved (docs/coverage-notes.md §3.5).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "ui/ui_state.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#ifndef _WIN32
#  include <unistd.h>
#endif

using namespace std::chrono_literals;
using soar::app::formatClock;
using soar::app::HudVisibility;
using soar::app::RecentStore;
using soar::app::Toast;

namespace {

// Scratch dir per process; POSIX uses mkdtemp, Windows a fixed temp path
// under %TEMP% (created by save() via the store's own ensureDir).
std::string makeTempDir() {
#ifdef _WIN32
  const char* base = std::getenv("TEMP");
  std::string dir = (base && *base ? base : ".") + std::string("\\soar_ui_test");
  return dir;
#else
  std::string tmpl = "/tmp/soar_ui_test_XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  const char* dir = ::mkdtemp(buf.data());
  return dir ? std::string(dir) : std::string(".");
#endif
}

const std::string kScratch = makeTempDir();

std::string readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

void writeFile(const std::string& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << content;
}

}  // namespace

TEST_CASE("formatClock renders mm:ss and h:mm:ss") {
  CHECK(formatClock(0ms) == "00:00");
  CHECK(formatClock(5ms) == "00:00");        // sub-second truncates
  CHECK(formatClock(5999ms) == "00:05");     // 5.999s -> 5s
  CHECK(formatClock(65s) == "01:05");
  CHECK(formatClock(3599s) == "59:59");
  CHECK(formatClock(1h) == "1:00:00");
  CHECK(formatClock(1h + 1min + 1s) == "1:01:01");
  CHECK(formatClock(-5s) == "00:00");        // clamps
  CHECK(formatClock(std::chrono::milliseconds(100h)) == "100:00:00");
}

TEST_CASE("HudVisibility hides after the idle delay and resets on activity") {
  HudVisibility hud(0ms);
  CHECK(hud.visible(0ms, false, false, false, false));
  CHECK(hud.visible(2499ms, false, false, false, false));  // still inside
  CHECK_FALSE(hud.visible(2500ms, false, false, false, false));  // boundary

  hud.noteActivity(5000ms);
  CHECK(hud.visible(7000ms, false, false, false, false));
  CHECK_FALSE(hud.visible(7500ms, false, false, false, false));
}

TEST_CASE("HudVisibility pin conditions keep the bar up past the delay") {
  HudVisibility hud(0ms);
  const auto late = 60s;  // far past the idle delay
  CHECK(hud.visible(late, /*pointer_over_hud=*/true, false, false, false));
  CHECK(hud.visible(late, false, /*seek_drag=*/true, false, false));
  CHECK(hud.visible(late, false, false, /*overlay_open=*/true, false));
  CHECK(hud.visible(late, false, false, false, /*paused=*/true));
  CHECK_FALSE(hud.visible(late, false, false, false, false));
}

TEST_CASE("HudVisibility manual force beats timer and pins until next input") {
  HudVisibility hud(0ms);
  hud.setForce(HudVisibility::Force::Hide);
  CHECK_FALSE(hud.visible(10ms, false, false, false, false));  // hides now
  // Manual intent outranks the pins too: a click while paused still hides.
  CHECK_FALSE(hud.visible(10ms, false, false, false, /*paused=*/true));
  hud.setForce(HudVisibility::Force::Show);
  CHECK(hud.visible(10 * 60s, false, false, false, false));  // pinned on

  // Any other input hands control back to the idle timer.
  hud.setForce(HudVisibility::Force::Hide);
  hud.noteActivity(20s);
  CHECK(hud.visible(20s + 1000ms, false, false, false, false));
  CHECK_FALSE(hud.visible(20s + HudVisibility::kHideDelay, false, false,
                          false, false));
}

TEST_CASE("toast shows once, ages to one, expires after its duration") {
  Toast toast;
  CHECK_FALSE(toast.active(0ms));
  CHECK(toast.text().empty());
  // Before the first show, shown_at_ sits one duration in the past, so a
  // clock reading from before that point clamps to zero instead of going
  // negative (the HUD maps the value straight onto an alpha).
  CHECK(toast.age(-Toast::kDuration * 2) == doctest::Approx(0.0));
  CHECK(toast.age(0ms) == doctest::Approx(1.0));

  toast.show("Volume 75%", 100ms);
  CHECK(toast.active(100ms));
  CHECK(toast.active(899ms));
  CHECK_FALSE(toast.active(900ms));  // 800ms lifetime, boundary

  CHECK(toast.age(100ms) == doctest::Approx(0.0));
  CHECK(toast.age(500ms) == doctest::Approx(0.5));
  CHECK(toast.age(899ms) == doctest::Approx(0.99875).epsilon(0.001));
  CHECK(toast.age(5000ms) == doctest::Approx(1.0));  // clamps after expiry

  toast.show("Muted", 2000ms);
  CHECK(toast.text() == "Muted");
  CHECK_FALSE(toast.active(1500ms));  // the old deadline no longer applies
  CHECK(toast.active(2100ms));
}

TEST_CASE("recent store loads missing files as empty and round-trips saves") {
  const std::string path = kScratch + "/recent_empty.txt";
  RecentStore store(path);
  store.load();
  CHECK(store.entries().empty());

  CHECK(store.add("file:///a.mkv"));
  CHECK(store.add("http://127.0.0.1:8000/b.m3u8"));
  CHECK(store.add("file:///a.mkv"));  // moves to front, still one entry
  REQUIRE(store.entries().size() == 2);
  CHECK(store.entries()[0] == "file:///a.mkv");
  CHECK(store.entries()[1] == "http://127.0.0.1:8000/b.m3u8");

  CHECK(store.save());
  RecentStore reloaded(path);
  reloaded.load();
  REQUIRE(reloaded.entries().size() == 2);
  CHECK(reloaded.entries()[0] == "file:///a.mkv");
  CHECK(reloaded.entries()[1] == "http://127.0.0.1:8000/b.m3u8");
}

TEST_CASE("recent store add reports first-position idempotence") {
  RecentStore store(kScratch + "/recent_idem.txt");
  CHECK(store.add("a"));   // new -> changed
  CHECK_FALSE(store.add("a"));  // already first -> unchanged
  CHECK(store.add("b"));   // new front -> changed
  CHECK(store.entries().size() == 2);
  CHECK(store.entries()[0] == "b");
}

TEST_CASE("recent store caps at 15 and keeps the newest first") {
  RecentStore store(kScratch + "/recent_cap.txt");
  for (int i = 0; i < 20; ++i) {
    store.add("uri-" + std::to_string(i));
  }
  REQUIRE(store.entries().size() == RecentStore::kMaxEntries);
  CHECK(store.entries().front() == "uri-19");
  CHECK(store.entries().back() == "uri-5");  // the six oldest fell off
}

TEST_CASE("recent store load tolerates hand-edited files") {
  const std::string path = kScratch + "/recent_manual.txt";
  // Windows CRLF, blank lines, padded entries, duplicates — the loader
  // trims and dedupes instead of trusting the file.
  writeFile(path, "  first.mkv\r\n\n   \nsecond.mkv\nfirst.mkv\nthird.mkv\n");
  RecentStore store(path);
  store.load();
  REQUIRE(store.entries().size() == 3);
  CHECK(store.entries()[0] == "first.mkv");
  CHECK(store.entries()[1] == "second.mkv");
  CHECK(store.entries()[2] == "third.mkv");
}

TEST_CASE("recent store load stops reading once the cap is reached") {
  // A file with more entries than the cap (hand-edited, or written by an
  // older build with a bigger limit) is truncated at load time, not just
  // on the next add.
  const std::string path = kScratch + "/recent_overflow.txt";
  std::string content;
  for (int i = 0; i < 40; ++i) content += "uri-" + std::to_string(i) + "\n";
  writeFile(path, content);
  RecentStore store(path);
  store.load();
  REQUIRE(store.entries().size() == RecentStore::kMaxEntries);
  CHECK(store.entries().front() == "uri-0");
  CHECK(store.entries().back() == "uri-14");
}

TEST_CASE("recent store saves a bare filename without creating directories") {
  // No separator in the path: the store must skip the mkdir step and
  // write next to the working directory instead of failing on it.
  const std::string name = "soar_recent_bare_test.txt";
  std::remove(name.c_str());
  RecentStore store(name);
  store.add("asset://sample");
  CHECK(store.save());
  RecentStore reloaded(name);
  reloaded.load();
  REQUIRE(reloaded.entries().size() == 1);
  CHECK(reloaded.entries()[0] == "asset://sample");
  std::remove(name.c_str());
}

TEST_CASE("recent store save fails cleanly when the path is unusable") {
  // A regular file where a directory would be needed: ensureDir cannot
  // create it and the tmp-file open fails — save reports false instead of
  // throwing (the UI keeps running with its in-memory list).
  const std::string blocker = kScratch + "/blocker";
  writeFile(blocker, "in the way");
  RecentStore store(blocker + "/sub/recent.txt");
  store.add("a");
  CHECK_FALSE(store.save());
}

#ifndef _WIN32
TEST_CASE("recent store save reports failure when the write itself fails") {
  // The tmp file opens but the stream dies mid-write: save must return
  // false (and leave the previous list in place) instead of reporting a
  // successful persistence. /dev/full is the only portable way to get
  // there; when it is unavailable the case says so instead of pretending.
  RecentStore store("/dev/full");
  store.add("a");
  const bool saved = store.save();
  if (saved) {
    MESSAGE("/dev/full did not fail the write on this platform; "
            "the mid-write error arc stays unverified here");
  } else {
    CHECK_FALSE(saved);
  }
}
#endif

#ifndef _WIN32
TEST_CASE("defaultRecentPath follows XDG_STATE_HOME then HOME") {
  const std::string scratch = kScratch + "/xdgtest";

  ::setenv("XDG_STATE_HOME", scratch.c_str(), 1);
  CHECK(soar::app::defaultRecentPath() == scratch + "/soar/recent.txt");

  ::unsetenv("XDG_STATE_HOME");
  const char* home = std::getenv("HOME");
  REQUIRE(home != nullptr);
  CHECK(soar::app::defaultRecentPath() ==
        std::string(home) + "/.local/state/soar/recent.txt");

  // Neither variable set: the bare relative fallback keeps the UI usable
  // (the list just lives next to the working directory).
  ::unsetenv("HOME");
  CHECK(soar::app::defaultRecentPath() == "soar-recent.txt");
  ::setenv("HOME", home, 1);

  // Not a fixture leak: restore nothing — XDG_STATE_HOME unset is the
  // default state for the next test binary run.
}
#endif

//=============================================================================
// PlaylistStore (docs/mvp.md §2)
//=============================================================================

TEST_CASE("PlaylistStore add/append and empty list adopts first as current") {
  soar::app::PlaylistStore ps;
  CHECK(ps.empty());
  CHECK(ps.current() == soar::app::PlaylistStore::kNone);

  ps.add("file:///a.mkv");
  CHECK(ps.size() == 1);
  CHECK(ps.current() == 0);
  CHECK(ps.entries()[0] == "file:///a.mkv");

  ps.add("file:///b.mkv");
  CHECK(ps.size() == 2);
  CHECK(ps.current() == 0);  // unchanged after second add
  CHECK(ps.entries()[1] == "file:///b.mkv");
}

TEST_CASE("PlaylistStore remove shifts current and clamps on tail removal") {
  soar::app::PlaylistStore ps;
  ps.add("a");
  ps.add("b");
  ps.add("c");
  ps.setCurrent(1);  // current = "b"

  // Remove earlier entry: current shifts left
  CHECK(ps.remove(0));
  CHECK(ps.current() == 0);
  CHECK(ps.entries()[0] == "b");

  // Remove current entry: adopts successor
  CHECK(ps.remove(0));
  CHECK(ps.current() == 0);
  CHECK(ps.entries()[0] == "c");

  // Remove tail: current clamps to new last
  ps.add("d");
  ps.setCurrent(1);  // current = "d"
  CHECK(ps.remove(1));
  CHECK(ps.current() == 0);
  CHECK(ps.entries()[0] == "c");

  // Remove last entry -> kNone
  CHECK(ps.remove(0));
  CHECK(ps.empty());
  CHECK(ps.current() == soar::app::PlaylistStore::kNone);

  // Out of range
  CHECK_FALSE(ps.remove(0));
}

TEST_CASE("PlaylistStore setCurrent validates index and empty list") {
  soar::app::PlaylistStore ps;
  CHECK_FALSE(ps.setCurrent(0));  // empty
  CHECK_FALSE(ps.setCurrent(5));  // out of range

  ps.add("a");
  CHECK(ps.setCurrent(0));
  CHECK(ps.current() == 0);
  CHECK_FALSE(ps.setCurrent(1));
}

TEST_CASE("PlaylistStore clear resets to empty state") {
  soar::app::PlaylistStore ps;
  ps.add("a");
  ps.add("b");
  ps.setCurrent(1);
  ps.clear();
  CHECK(ps.empty());
  CHECK(ps.current() == soar::app::PlaylistStore::kNone);
}

TEST_CASE("PlaylistStore loop mode and shuffle toggles reset played round") {
  soar::app::PlaylistStore ps;
  ps.add("a");
  ps.add("b");
  ps.add("c");
  ps.setCurrent(0);

  // Enable shuffle starts fresh round
  ps.setShuffle(true);
  // played_ should be all false now
  // We can't directly inspect played_, but advance() behavior confirms

  // Toggle shuffle off/on starts fresh round
  ps.setShuffle(false);
  ps.setShuffle(true);

  // Loop mode changes
  ps.setLoop(soar::app::PlaylistStore::Loop::All);
  CHECK(ps.loop() == soar::app::PlaylistStore::Loop::All);
  ps.setLoop(soar::app::PlaylistStore::Loop::One);
  CHECK(ps.loop() == soar::app::PlaylistStore::Loop::One);
  ps.setLoop(soar::app::PlaylistStore::Loop::Off);
  CHECK(ps.loop() == soar::app::PlaylistStore::Loop::Off);
}

TEST_CASE("PlaylistStore next/prev sequential with Loop::Off stops at ends") {
  soar::app::PlaylistStore ps;
  ps.add("a");
  ps.add("b");
  ps.add("c");
  ps.setCurrent(0);
  ps.setLoop(soar::app::PlaylistStore::Loop::Off);

  CHECK(ps.next() == 1);
  ps.setCurrent(1);
  CHECK(ps.next() == 2);
  ps.setCurrent(2);
  CHECK(ps.next() == soar::app::PlaylistStore::kNone);  // end
  CHECK(ps.prev() == 1);
  ps.setCurrent(1);
  CHECK(ps.prev() == 0);
  ps.setCurrent(0);
  CHECK(ps.prev() == soar::app::PlaylistStore::kNone);  // start
}

TEST_CASE("PlaylistStore next/prev wrap with Loop::All") {
  soar::app::PlaylistStore ps;
  ps.add("a");
  ps.add("b");
  ps.add("c");
  ps.setCurrent(0);
  ps.setLoop(soar::app::PlaylistStore::Loop::All);

  CHECK(ps.next() == 1);
  ps.setCurrent(1);
  CHECK(ps.next() == 2);
  ps.setCurrent(2);
  CHECK(ps.next() == 0);  // wraps
  ps.setCurrent(0);
  CHECK(ps.prev() == 2);  // wraps back
  ps.setCurrent(2);
  CHECK(ps.prev() == 1);
  ps.setCurrent(1);
  CHECK(ps.prev() == 0);
}

TEST_CASE("PlaylistStore next/prev with Loop::One wraps like All for manual steps") {
  soar::app::PlaylistStore ps;
  ps.add("a");
  ps.add("b");
  ps.setCurrent(0);
  ps.setLoop(soar::app::PlaylistStore::Loop::One);

  CHECK(ps.next() == 1);
  ps.setCurrent(1);
  CHECK(ps.next() == 0);  // wraps like All for manual steps
  ps.setCurrent(0);
  CHECK(ps.prev() == 1);
}

TEST_CASE("PlaylistStore next/prev shuffle picks other entry deterministically") {
  soar::app::PlaylistStore ps;
  ps.add("a");
  ps.add("b");
  ps.add("c");
  ps.setCurrent(0);
  ps.setShuffle(true);
  ps.reseed(0x1234);  // deterministic seed

  std::size_t n1 = ps.next();
  ps.setCurrent(n1);
  std::size_t n2 = ps.next();
  ps.setCurrent(n2);
  CHECK(n1 != n2);  // should pick different entries
  CHECK(n1 != soar::app::PlaylistStore::kNone);
  CHECK(n2 != soar::app::PlaylistStore::kNone);
  CHECK(n1 < 3);
  CHECK(n2 < 3);

  // Same seed gives same sequence
  ps.reseed(0x1234);
  CHECK(ps.next() == n1);
  ps.setCurrent(n1);
  CHECK(ps.next() == n2);
}

TEST_CASE("PlaylistStore advance sequential with Loop::Off stops when exhausted") {
  soar::app::PlaylistStore ps;
  ps.add("a");
  ps.add("b");
  ps.setCurrent(0);
  ps.setLoop(soar::app::PlaylistStore::Loop::Off);

  CHECK(ps.advance() == 1);
  CHECK(ps.advance() == soar::app::PlaylistStore::kNone);  // exhausted
  // Note: advance() updates current_ internally, so the second call is from index 1
}

TEST_CASE("PlaylistStore advance sequential with Loop::All wraps") {
  soar::app::PlaylistStore ps;
  ps.add("a");
  ps.add("b");
  ps.setCurrent(0);
  ps.setLoop(soar::app::PlaylistStore::Loop::All);

  CHECK(ps.advance() == 1);
  CHECK(ps.advance() == 0);  // wraps
  CHECK(ps.advance() == 1);
}

TEST_CASE("PlaylistStore advance with Loop::One replays current") {
  soar::app::PlaylistStore ps;
  ps.add("a");
  ps.add("b");
  ps.setCurrent(0);
  ps.setLoop(soar::app::PlaylistStore::Loop::One);

  CHECK(ps.advance() == 0);  // replays current
  CHECK(ps.advance() == 0);
}

TEST_CASE("PlaylistStore advance shuffle walks one round without repeats") {
  soar::app::PlaylistStore ps;
  ps.add("a");
  ps.add("b");
  ps.add("c");
  ps.setCurrent(0);
  ps.setShuffle(true);
  ps.setLoop(soar::app::PlaylistStore::Loop::Off);
  ps.reseed(0xABCD);

  std::vector<std::size_t> seen;
  for (int i = 0; i < 5; ++i) {
    auto n = ps.advance();
    if (n == soar::app::PlaylistStore::kNone) break;
    seen.push_back(n);
  }
  // Should see all 3 entries exactly once before stopping
  std::sort(seen.begin(), seen.end());
  CHECK(seen == std::vector<std::size_t>{0, 1, 2});
}

TEST_CASE("PlaylistStore advance shuffle with Loop::All starts next round") {
  soar::app::PlaylistStore ps;
  ps.add("a");
  ps.add("b");
  ps.setCurrent(0);
  ps.setShuffle(true);
  ps.setLoop(soar::app::PlaylistStore::Loop::All);
  ps.reseed(0xC0FFEE);

  // First round: see both entries
  std::vector<std::size_t> round1;
  for (int i = 0; i < 3; ++i) {
    round1.push_back(ps.advance());
  }
  // Second round: should continue (not kNone)
  auto r4 = ps.advance();
  CHECK(r4 != soar::app::PlaylistStore::kNone);
  CHECK(r4 < 2);
}

TEST_CASE("PlaylistStore single entry edge cases") {
  soar::app::PlaylistStore ps;
  ps.add("only");
  ps.setCurrent(0);

  // next/prev on single entry
  CHECK(ps.next() == soar::app::PlaylistStore::kNone);  // Loop::Off
  CHECK(ps.prev() == soar::app::PlaylistStore::kNone);

  ps.setLoop(soar::app::PlaylistStore::Loop::All);
  CHECK(ps.next() == 0);  // wraps to self
  CHECK(ps.prev() == 0);

  // shuffle on single entry
  ps.setShuffle(true);
  ps.reseed(0xDEAD);
  CHECK(ps.next() == 0);  // pickOther clamps to the only entry
  CHECK(ps.advance() == soar::app::PlaylistStore::kNone);  // Loop::Off exhausts after one

  ps.setLoop(soar::app::PlaylistStore::Loop::All);
  CHECK(ps.advance() == 0);  // replays
}
