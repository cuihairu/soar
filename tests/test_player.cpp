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

  CHECK(fx.player.state() == soar::PlaybackState::Stopped);
  CHECK_FALSE(fx.player.lastError().empty());
  CHECK(fx.log.countOf(soar::EventType::Error) >= 6);
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
  CHECK_FALSE(player.seek(1s));
  CHECK(player.state() == soar::PlaybackState::Stopped);
  CHECK(player.mediaInfo().tracks.empty());
  CHECK(player.position() == 0ms);
  CHECK(player.lastError().empty());
}

TEST_CASE("events without a callback are safe") {
  soar::Player player{soar::makeNullBackend()};
  player.setEventCallback(nullptr);

  CHECK(player.open(soar::MediaSource{"asset://sample"}));
  CHECK(player.play());
  CHECK(player.stop());
}
