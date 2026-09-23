// Functional tests for FFmpegBackend's public surface: event delivery,
// unopened-state behavior of every control call, and (with real media)
// the open/play/seek lifecycle against the counting event sink.
//
// The concurrency storms live in test_backend_concurrency.cpp; this file
// covers semantics. Media-dependent cases need SOAR_TEST_MEDIA (CI
// generates a dual-audio sample); without it they report a message and
// return early.

#ifdef SOAR_WITH_FFMPEG
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "soar/core/ffmpeg_backend.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace {

bool mediaAvailable(std::string& out_path) {
  const char* media = std::getenv("SOAR_TEST_MEDIA");
  if (!media || !*media) {
    return false;
  }
  std::ifstream f(media);
  if (!f.good()) {
    return false;
  }
  out_path = media;
  return true;
}

struct CountingSink : soar::IEventSink {
  std::atomic<int> state_changed{0};
  std::atomic<int> media_info_changed{0};
  std::atomic<int> position_changed{0};
  std::atomic<int> errors{0};

  void onEvent(const soar::Event& e) override {
    switch (e.type) {
      case soar::EventType::StateChanged: ++state_changed; break;
      case soar::EventType::MediaInfoChanged: ++media_info_changed; break;
      case soar::EventType::PositionChanged: ++position_changed; break;
      case soar::EventType::Error: ++errors; break;
    }
  }
};

} // namespace

TEST_CASE("unopened backend: control surface semantics") {
  auto backend = soar::makeFFmpegBackend();
  CountingSink sink;
  backend->setEventSink(&sink);

  // Every control call on an unopened backend fails and reports why.
  CHECK_FALSE(backend->play());
  CHECK_FALSE(backend->pause());
  CHECK_FALSE(backend->stop());
  CHECK_FALSE(backend->seek(100ms));
  CHECK_FALSE(backend->selectTrack(soar::TrackType::Audio, 0));
  CHECK_FALSE(backend->disableSubtitles());
  CHECK_FALSE(backend->setRate(0.0));
  CHECK_FALSE(backend->setRate(-1.0));
  CHECK(backend->lastError().empty() == false);
  CHECK(sink.errors.load() >= 7);

  // Rate/volume/mute are pure playback parameters: they succeed without
  // media (rate must be positive; volume is clamped into [0, 1]).
  CHECK(backend->setRate(1.5));
  CHECK(backend->setVolume(0.5));
  CHECK(backend->setVolume(42.0)); // clamped, still succeeds
  CHECK(backend->setMuted(true));
  CHECK(backend->setMuted(false));

  // Unopened state is all zeros / empty.
  CHECK(backend->state() == soar::PlaybackState::Stopped);
  CHECK(backend->position() == 0ms);
  CHECK(backend->mediaInfo().tracks.empty());
  CHECK_FALSE(backend->mediaInfo().seekable);
  soar::FFmpegBackend::DecodedVideoFrame frame;
  CHECK_FALSE(backend->tryGetVideoFrame(frame));

  // open() on a missing file fails (fatal path: state becomes Error) and
  // records the path in the error.
  CHECK_FALSE(backend->open(soar::MediaSource{"/definitely/missing/file.mp4"}));
  CHECK(backend->lastError().find("/definitely/missing/file.mp4") != std::string::npos);
  CHECK(backend->state() == soar::PlaybackState::Error);
  CHECK(backend->mediaInfo().tracks.empty());

  // close() on an unopened backend is a no-op, not an error.
  backend->close();
  CHECK(backend->state() == soar::PlaybackState::Stopped);
  CHECK(backend->position() == 0ms);
}

TEST_CASE("media lifecycle drives events, position and seek") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping media lifecycle test");
    return;
  }

  auto backend = soar::makeFFmpegBackend();
  CountingSink sink;
  backend->setEventSink(&sink);

  REQUIRE(backend->open(soar::MediaSource{media}));

  const auto info = backend->mediaInfo();
  CHECK_FALSE(info.tracks.empty());
  CHECK(info.duration > 0ms);
  CHECK(info.seekable);
  CHECK(sink.media_info_changed.load() >= 1);

  REQUIRE(backend->play());
  CHECK(backend->state() == soar::PlaybackState::Playing);
  CHECK(sink.state_changed.load() >= 1);

  // The playback clock runs while playing.
  std::this_thread::sleep_for(300ms);
  CHECK(backend->position() > 0ms);

  // PositionChanged events stream at the emit granularity (200ms).
  CHECK(sink.position_changed.load() >= 1);

  // Pausing freezes the clock; the position stays put.
  CHECK(backend->pause());
  const auto frozen = backend->position();
  CHECK(backend->state() == soar::PlaybackState::Paused);
  std::this_thread::sleep_for(150ms);
  CHECK(backend->position() == frozen);

  // Seeking while paused posts the request to the decode thread; the
  // position converges on the target asynchronously.
  CHECK(backend->seek(1000ms));
  bool seeked = false;
  for (int i = 0; i < 200 && !seeked; ++i) {
    seeked = backend->position() == 1000ms;
    if (!seeked) {
      std::this_thread::sleep_for(10ms);
    }
  }
  CHECK(seeked);
  CHECK(backend->play());
  CHECK(backend->state() == soar::PlaybackState::Playing);

  CHECK(backend->stop());
  CHECK(backend->state() == soar::PlaybackState::Stopped);
  CHECK(backend->position() == 0ms);

  backend->close();
  CHECK(backend->mediaInfo().tracks.empty());
  CHECK(backend->position() == 0ms);
}

TEST_CASE("subtitles default to off and disableSubtitles is metadata-only") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping subtitle metadata test");
    return;
  }

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{media}));

  CHECK(backend->mediaInfo().selected_subtitle == -1);

  // Picking a real subtitle track (if any) is metadata-only and works
  // while stopped; disabling returns to the off state.
  const auto& tracks = backend->mediaInfo().tracks;
  for (const auto& t : tracks) {
    if (t.type == soar::TrackType::Subtitle) {
      CHECK(backend->selectTrack(soar::TrackType::Subtitle, t.id));
      CHECK(backend->mediaInfo().selected_subtitle == t.id);
      break;
    }
  }
  CHECK(backend->disableSubtitles());
  CHECK(backend->mediaInfo().selected_subtitle == -1);

  backend->close();
}

#else // !SOAR_WITH_FFMPEG

// Keep the test binary meaningful when the FFmpeg backend is not compiled.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

TEST_CASE("ffmpeg backend media tests require SOAR_WITH_FFMPEG") {
  MESSAGE("FFmpeg backend not compiled in; nothing to test here");
}

#endif
