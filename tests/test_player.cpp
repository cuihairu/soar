// Unit tests for the Player facade driven by the NullBackend test double.
// These tests exercise the core playback API state machine without any
// multimedia dependencies, so they run on every platform and in every CI job.
//
// 行为参照：src/core/null_backend.cpp（NullBackend 的确定性状态机）。

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "soar/core/player.h"

#include <chrono>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace {

class EventLog {
public:
  void attach(soar::Player& player) {
    player.setEventCallback([this](const soar::Event& e) { events_.push_back(e); });
  }

  const std::vector<soar::Event>& events() const {
    return events_;
  }

  std::size_t countOf(soar::EventType type) const {
    std::size_t n = 0;
    for (const auto& e : events_) {
      if (e.type == type) {
        ++n;
      }
    }
    return n;
  }

  void clear() {
    events_.clear();
  }

private:
  std::vector<soar::Event> events_;
};

struct Fixture {
  EventLog log;
  soar::Player player{soar::makeNullBackend()};

  Fixture() {
    log.attach(player);
  }
};

} // namespace

TEST_CASE("open builds media info and reports initial state") {
  Fixture fx;

  CHECK(fx.player.open(soar::MediaSource{"asset://sample"}));
  CHECK(fx.player.state() == soar::PlaybackState::Stopped);

  const auto info = fx.player.mediaInfo();
  CHECK(info.duration == 10min);
  CHECK(info.seekable);
  CHECK(info.tracks.size() == 3);
  CHECK(info.selected_video == 0);
  CHECK(info.selected_audio == 1);
  CHECK(info.selected_subtitle == -1);

  CHECK(fx.log.countOf(soar::EventType::MediaInfoChanged) >= 1);
  CHECK(fx.log.countOf(soar::EventType::StateChanged) >= 1);
  CHECK(fx.player.lastError().empty());
}

TEST_CASE("commands before open fail with an error event") {
  Fixture fx;

  CHECK_FALSE(fx.player.play());
  CHECK_FALSE(fx.player.pause());
  CHECK_FALSE(fx.player.stop());
  CHECK_FALSE(fx.player.seek(1s));
  CHECK_FALSE(fx.player.selectTrack(soar::TrackType::Audio, 1));
  CHECK_FALSE(fx.player.disableSubtitles());
  std::chrono::milliseconds loop_a{0}, loop_b{0};
  CHECK_FALSE(fx.player.setLoopAB(0ms, 1s));
  CHECK(fx.player.lastError().find("setLoopAB") != std::string::npos);
  CHECK_FALSE(fx.player.clearLoopAB());
  CHECK_FALSE(fx.player.loopAB(loop_a, loop_b));

  CHECK(fx.player.state() == soar::PlaybackState::Stopped);
  CHECK_FALSE(fx.player.lastError().empty());
  CHECK(fx.log.countOf(soar::EventType::Error) >= 8);
}

TEST_CASE("play and pause drive the state machine") {
  Fixture fx;
  REQUIRE(fx.player.open(soar::MediaSource{"asset://sample"}));
  fx.log.clear();

  CHECK(fx.player.play());
  CHECK(fx.player.state() == soar::PlaybackState::Playing);

  CHECK(fx.player.play()); // repeated play is a no-op success
  CHECK(fx.player.state() == soar::PlaybackState::Playing);

  CHECK(fx.player.pause());
  CHECK(fx.player.state() == soar::PlaybackState::Paused);
  CHECK(fx.log.countOf(soar::EventType::StateChanged) >= 3);
}

TEST_CASE("stop resets position and returns to Stopped") {
  Fixture fx;
  REQUIRE(fx.player.open(soar::MediaSource{"asset://sample"}));
  REQUIRE(fx.player.play());

  CHECK(fx.player.stop());
  CHECK(fx.player.state() == soar::PlaybackState::Stopped);
  CHECK(fx.player.position() == 0ms);
  CHECK(fx.log.countOf(soar::EventType::PositionChanged) >= 1);
}

TEST_CASE("seek clamps to the media duration") {
  Fixture fx;
  REQUIRE(fx.player.open(soar::MediaSource{"asset://sample"}));

  CHECK(fx.player.seek(-5s));
  CHECK(fx.player.position() == 0ms);

  CHECK(fx.player.seek(600s)); // beyond the 10min duration
  CHECK(fx.player.position() == 10min);
  CHECK(fx.player.state() == soar::PlaybackState::Ended);
}

TEST_CASE("seeking to the end while already Ended does not re-announce the state") {
  Fixture fx;
  REQUIRE(fx.player.open(soar::MediaSource{"asset://sample"}));
  REQUIRE(fx.player.seek(fx.player.mediaInfo().duration));
  REQUIRE(fx.player.state() == soar::PlaybackState::Ended);

  const auto announced = fx.log.countOf(soar::EventType::StateChanged);
  CHECK(fx.player.seek(fx.player.mediaInfo().duration));
  CHECK(fx.player.position() == 10min);
  CHECK(fx.player.state() == soar::PlaybackState::Ended);
  // The state did not change, so no additional StateChanged may be emitted.
  CHECK(fx.log.countOf(soar::EventType::StateChanged) == announced);
}

TEST_CASE("seek away from the end resumes to Paused") {
  Fixture fx;
  REQUIRE(fx.player.open(soar::MediaSource{"asset://sample"}));
  REQUIRE(fx.player.seek(fx.player.mediaInfo().duration));
  REQUIRE(fx.player.state() == soar::PlaybackState::Ended);

  CHECK(fx.player.seek(5s));
  CHECK(fx.player.position() == 5s);
  CHECK(fx.player.state() == soar::PlaybackState::Paused);
}

TEST_CASE("play after Ended restarts from the beginning") {
  Fixture fx;
  REQUIRE(fx.player.open(soar::MediaSource{"asset://sample"}));
  REQUIRE(fx.player.seek(fx.player.mediaInfo().duration));
  REQUIRE(fx.player.state() == soar::PlaybackState::Ended);

  CHECK(fx.player.play());
  CHECK(fx.player.state() == soar::PlaybackState::Playing);
  CHECK(fx.player.position() == 0ms);
}

TEST_CASE("setRate rejects non-positive values") {
  Fixture fx;
  REQUIRE(fx.player.open(soar::MediaSource{"asset://sample"}));

  CHECK(fx.player.setRate(0.5));
  CHECK(fx.player.setRate(2.0));
  CHECK_FALSE(fx.player.setRate(0.0));
  CHECK_FALSE(fx.player.setRate(-1.0));
  CHECK_FALSE(fx.player.lastError().empty());
  CHECK(fx.log.countOf(soar::EventType::Error) == 2);
}

TEST_CASE("setVolume and setMuted accept the configured ranges") {
  Fixture fx;
  REQUIRE(fx.player.open(soar::MediaSource{"asset://sample"}));

  CHECK(fx.player.setVolume(0.0));
  CHECK(fx.player.setVolume(1.0));
  CHECK(fx.player.setVolume(0.25));
  CHECK(fx.player.setMuted(true));
  CHECK(fx.player.setMuted(false));
}

TEST_CASE("A-B loop arms, queries and clears through the facade") {
  Fixture fx;
  REQUIRE(fx.player.open(soar::MediaSource{"asset://sample"}));

  // Invalid windows are rejected and never leave a half-armed state.
  std::chrono::milliseconds a{0}, b{0};
  CHECK_FALSE(fx.player.setLoopAB(2000ms, 1000ms));
  CHECK_FALSE(fx.player.setLoopAB(-1ms, 1000ms));
  CHECK_FALSE(fx.player.loopAB(a, b));
  CHECK_FALSE(fx.player.lastError().empty());

  CHECK(fx.player.setLoopAB(1500ms, 65000ms));
  REQUIRE(fx.player.loopAB(a, b));
  CHECK(a == 1500ms);
  CHECK(b == 65000ms);

  // stop() keeps the window; clearing drops it.
  REQUIRE(fx.player.play());
  CHECK(fx.player.stop());
  CHECK(fx.player.loopAB(a, b));
  CHECK(fx.player.clearLoopAB());
  CHECK_FALSE(fx.player.loopAB(a, b));
}

TEST_CASE("A-B loop rejects unseekable sources and survives close") {
  Fixture fx;
  REQUIRE(fx.player.open(soar::MediaSource{"asset://noseek"}));

  CHECK_FALSE(fx.player.setLoopAB(0ms, 1000ms));
  CHECK(fx.player.lastError().find("setLoopAB") != std::string::npos);
  // Clearing needs no seekable media: it is a safe no-op when disarmed.
  CHECK(fx.player.clearLoopAB());

  // An armed window on a seekable source is torn down by close().
  REQUIRE(fx.player.open(soar::MediaSource{"asset://sample"}));
  REQUIRE(fx.player.setLoopAB(0ms, 60000ms));
  fx.player.close();
  std::chrono::milliseconds a{0}, b{0};
  CHECK_FALSE(fx.player.loopAB(a, b));
}

TEST_CASE("selectTrack validates ids and updates media info") {
  Fixture fx;
  REQUIRE(fx.player.open(soar::MediaSource{"asset://sample"}));

  CHECK(fx.player.selectTrack(soar::TrackType::Audio, 1));
  CHECK(fx.player.mediaInfo().selected_audio == 1);

  CHECK(fx.player.selectTrack(soar::TrackType::Subtitle, 2));
  CHECK(fx.player.mediaInfo().selected_subtitle == 2);

  CHECK_FALSE(fx.player.selectTrack(soar::TrackType::Audio, 99));
  CHECK_FALSE(fx.player.selectTrack(soar::TrackType::Video, 0));
  CHECK(fx.log.countOf(soar::EventType::MediaInfoChanged) >= 2);
}

TEST_CASE("disableSubtitles clears the selection") {
  Fixture fx;
  REQUIRE(fx.player.open(soar::MediaSource{"asset://sample"}));
  REQUIRE(fx.player.selectTrack(soar::TrackType::Subtitle, 2));

  CHECK(fx.player.disableSubtitles());
  CHECK(fx.player.mediaInfo().selected_subtitle == -1);
}

TEST_CASE("events carry the state and position at emit time") {
  Fixture fx;
  REQUIRE(fx.player.open(soar::MediaSource{"asset://sample"}));
  fx.log.clear();

  REQUIRE(fx.player.play());
  REQUIRE(fx.player.stop());

  // The last event of stop() is the PositionChanged emitted after the state
  // reset, so it must report Stopped and position 0.
  const auto& events = fx.log.events();
  REQUIRE(events.size() >= 2);
  const auto& last = events.back();
  CHECK(last.type == soar::EventType::PositionChanged);
  CHECK(last.state == soar::PlaybackState::Stopped);
  CHECK(last.position == 0ms);
}

TEST_CASE("close clears media info") {
  Fixture fx;
  REQUIRE(fx.player.open(soar::MediaSource{"asset://sample"}));

  fx.player.close();
  CHECK(fx.player.state() == soar::PlaybackState::Stopped);
  CHECK(fx.player.mediaInfo().tracks.empty());
  CHECK(fx.player.position() == 0ms);
}

TEST_CASE("player without a backend fails safely") {
  soar::Player player{nullptr};

  CHECK_FALSE(player.open(soar::MediaSource{"asset://sample"}));
  CHECK_FALSE(player.play());
  CHECK_FALSE(player.pause());
  CHECK_FALSE(player.stop());
  CHECK_FALSE(player.seek(1s));
  CHECK_FALSE(player.setRate(1.5));
  CHECK_FALSE(player.setVolume(0.5));
  CHECK_FALSE(player.setMuted(true));
  CHECK_FALSE(player.selectTrack(soar::TrackType::Audio, 1));
  CHECK_FALSE(player.disableSubtitles());
  std::chrono::milliseconds loop_a{0}, loop_b{0};
  CHECK_FALSE(player.setLoopAB(0ms, 1s));
  CHECK_FALSE(player.clearLoopAB());
  CHECK_FALSE(player.loopAB(loop_a, loop_b));
  player.close(); // no backend: must be a no-op, not a crash

  CHECK(player.state() == soar::PlaybackState::Stopped);
  CHECK(player.mediaInfo().tracks.empty());
  CHECK(player.position() == 0ms);
  CHECK(player.lastError().empty());
}

TEST_CASE("backend without a sink drops events safely") {
  // A raw backend that was never given a sink must simply discard its
  // events instead of dereferencing a null pointer.
  auto backend = soar::makeNullBackend();
  CHECK(backend->open(soar::MediaSource{"asset://sample"}));
  CHECK(backend->play());
  CHECK(backend->seek(2s));
  CHECK(backend->stop());
  backend->close();
  CHECK(backend->state() == soar::PlaybackState::Stopped);
}

TEST_CASE("seeking a non-seekable source fails") {
  Fixture fx;
  REQUIRE(fx.player.open(soar::MediaSource{"asset://noseek"}));

  const auto info = fx.player.mediaInfo();
  CHECK_FALSE(info.seekable);

  CHECK_FALSE(fx.player.seek(1s));
  CHECK_FALSE(fx.player.lastError().empty());
  CHECK(fx.player.position() == 0ms);
  CHECK(fx.log.countOf(soar::EventType::Error) == 1);

  // Play/pause and stop are unaffected by seekability.
  CHECK(fx.player.play());
  CHECK(fx.player.state() == soar::PlaybackState::Playing);
  CHECK(fx.player.stop());
}

TEST_CASE("a failed open reports the error and the player stays usable") {
  Fixture fx;
  const auto baseline = fx.log.events().size();

  CHECK_FALSE(fx.player.open(soar::MediaSource{"asset://fail-open.mp4"}));
  CHECK_FALSE(fx.player.lastError().empty());
  CHECK(fx.player.state() == soar::PlaybackState::Stopped);
  CHECK(fx.player.mediaInfo().tracks.empty());
  CHECK(fx.log.countOf(soar::EventType::Error) == 1);
  CHECK(fx.log.events().size() > baseline);

  // The same player recovers and opens a good source afterwards.
  REQUIRE(fx.player.open(soar::MediaSource{"asset://sample"}));
  CHECK(fx.player.mediaInfo().duration == 10min);
  CHECK(fx.player.play());
  CHECK(fx.player.state() == soar::PlaybackState::Playing);
}

TEST_CASE("events without a callback are safe") {
  soar::Player player{soar::makeNullBackend()};
  player.setEventCallback(nullptr);

  CHECK(player.open(soar::MediaSource{"asset://sample"}));
  CHECK(player.play());
  CHECK(player.stop());
}

TEST_CASE("close and reopen rebuild media info and reset selection") {
  Fixture fx;
  REQUIRE(fx.player.open(soar::MediaSource{"asset://sample"}));
  REQUIRE(fx.player.selectTrack(soar::TrackType::Subtitle, 2));
  REQUIRE(fx.player.mediaInfo().selected_subtitle == 2);

  fx.player.close();
  const auto clears = fx.log.countOf(soar::EventType::MediaInfoChanged);

  REQUIRE(fx.player.open(soar::MediaSource{"asset://sample"}));
  const auto info = fx.player.mediaInfo();
  CHECK(info.duration == 10min);
  CHECK(info.selected_video == 0);
  CHECK(info.selected_audio == 1);
  CHECK(info.selected_subtitle == -1); // selection does not survive reopen

  // close() and the fresh open() each announced the (reset) media info.
  CHECK(fx.log.countOf(soar::EventType::MediaInfoChanged) >= clears + 1);

  // The reopened media plays from the start.
  CHECK(fx.player.play());
  CHECK(fx.player.state() == soar::PlaybackState::Playing);
}

TEST_CASE("seeking from Ended to the lower bound resumes to Paused") {
  Fixture fx;
  REQUIRE(fx.player.open(soar::MediaSource{"asset://sample"}));
  REQUIRE(fx.player.seek(fx.player.mediaInfo().duration));
  REQUIRE(fx.player.state() == soar::PlaybackState::Ended);

  CHECK(fx.player.seek(0ms));
  CHECK(fx.player.position() == 0ms);
  CHECK(fx.player.state() == soar::PlaybackState::Paused);
}

TEST_CASE("seeking while paused stays paused") {
  Fixture fx;
  REQUIRE(fx.player.open(soar::MediaSource{"asset://sample"}));
  REQUIRE(fx.player.pause());

  CHECK(fx.player.seek(5s));
  CHECK(fx.player.position() == 5s);
  CHECK(fx.player.state() == soar::PlaybackState::Paused);
}

TEST_CASE("selectTrack while playing is idempotent and keeps playing") {
  Fixture fx;
  REQUIRE(fx.player.open(soar::MediaSource{"asset://sample"}));
  REQUIRE(fx.player.play());

  CHECK(fx.player.selectTrack(soar::TrackType::Audio, 1));
  CHECK(fx.player.state() == soar::PlaybackState::Playing);
  CHECK(fx.player.mediaInfo().selected_audio == 1);

  // Selecting the same track again succeeds and re-announces the info.
  const auto before = fx.log.countOf(soar::EventType::MediaInfoChanged);
  CHECK(fx.player.selectTrack(soar::TrackType::Audio, 1));
  CHECK(fx.player.state() == soar::PlaybackState::Playing);
  CHECK(fx.log.countOf(soar::EventType::MediaInfoChanged) == before + 1);
}

TEST_CASE("rebinding the event callback redirects delivery") {
  EventLog log_a;
  EventLog log_b;
  soar::Player player{soar::makeNullBackend()};

  log_a.attach(player);
  CHECK_FALSE(player.play()); // unopened -> error event reaches log_a only
  CHECK(log_a.countOf(soar::EventType::Error) == 1);
  CHECK(log_b.events().empty());

  // Rebinding redirects delivery to the new callback only.
  log_b.attach(player);

  const auto frozen_a = log_a.events().size();
  CHECK_FALSE(player.pause());
  CHECK(log_b.countOf(soar::EventType::Error) >= 1);
  CHECK(log_a.events().size() == frozen_a);

  // Detaching the callback stops delivery entirely while calls still fail.
  player.setEventCallback(nullptr);
  const auto frozen_b = log_b.events().size();
  CHECK_FALSE(player.stop());
  CHECK(log_b.events().size() == frozen_b);
  CHECK(log_a.events().size() == frozen_a);
}

TEST_CASE("local playback never emits buffering events") {
  // BufferingStarted/Ended are network-stall signals from the FFmpeg
  // backend's decode loop. The null backend (and any local-file flow it
  // stands in for) must never produce them — this pins the contract that
  // buffering reporting costs local playback nothing.
  Fixture fx;
  fx.player.open(soar::MediaSource{"asset://sample"});
  fx.player.play();
  fx.player.seek(std::chrono::milliseconds{1000});
  fx.player.pause();
  fx.player.seek(std::chrono::milliseconds{2000});
  fx.player.play();
  fx.player.stop();

  CHECK(fx.log.countOf(soar::EventType::BufferingStarted) == 0);
  CHECK(fx.log.countOf(soar::EventType::BufferingEnded) == 0);
  CHECK(fx.log.countOf(soar::EventType::StateChanged) > 0);
}
