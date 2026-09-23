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
  // sink declared first: it must outlive the backend, whose destructor
  // still emits close events.
  CountingSink sink;
  auto backend = soar::makeFFmpegBackend();
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
  // tryGetVideoFrame is an FFmpegBackend extension beyond IBackend.
  soar::FFmpegBackend::DecodedVideoFrame frame;
  CHECK_FALSE(static_cast<soar::FFmpegBackend*>(backend.get())->tryGetVideoFrame(frame));

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

  // sink declared first: it must outlive the backend, whose destructor
  // still emits close events.
  CountingSink sink;
  auto backend = soar::makeFFmpegBackend();
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

TEST_CASE("seek clamps to media bounds and Ended recovers via play") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping seek bounds test");
    return;
  }

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{media}));
  const auto duration = backend->mediaInfo().duration;
  REQUIRE(duration > 0ms);

  // Out-of-range targets clamp into [0, duration]. From the stopped state
  // the seek executes synchronously on the calling thread.
  CHECK(backend->seek(-5000ms));
  CHECK(backend->position() == 0ms);

  // Seeking to the end lands exactly on the last timestamp state.
  CHECK(backend->seek(duration + 60000ms));
  CHECK(backend->position() == duration);
  CHECK(backend->state() == soar::PlaybackState::Ended);

  // Playing again restarts the media from the beginning.
  CHECK(backend->play());
  CHECK(backend->state() == soar::PlaybackState::Playing);
  CHECK(backend->position() < 500ms);

  // The full EOF loop while playing: seek to the end, let the decode
  // thread run out of data, observe Ended, then restart.
  REQUIRE(backend->seek(duration));
  bool ended = false;
  for (int i = 0; i < 500 && !ended; ++i) {
    ended = backend->state() == soar::PlaybackState::Ended;
    if (!ended) {
      std::this_thread::sleep_for(10ms);
    }
  }
  CHECK(ended);

  CHECK(backend->play());
  CHECK(backend->state() == soar::PlaybackState::Playing);
  CHECK(backend->position() < 500ms);

  backend->stop();
  backend->close();
}

TEST_CASE("setRate during playback keeps the clock moving forward") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping setRate test");
    return;
  }

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{media}));
  REQUIRE(backend->play());
  std::this_thread::sleep_for(200ms);
  const auto before = backend->position();

  // Doubling the speed re-bases the clock; position keeps advancing and
  // never jumps backwards.
  CHECK(backend->setRate(2.0));
  CHECK(backend->state() == soar::PlaybackState::Playing);
  std::this_thread::sleep_for(300ms);
  const auto faster = backend->position();
  CHECK(faster > before);

  // Halving again stays monotonic; an invalid rate is rejected without
  // disturbing playback.
  CHECK(backend->setRate(0.5));
  CHECK(backend->state() == soar::PlaybackState::Playing);
  bool advanced = false;
  for (int i = 0; i < 300 && !advanced; ++i) {
    advanced = backend->position() > faster;
    if (!advanced) {
      std::this_thread::sleep_for(10ms);
    }
  }
  CHECK(advanced);
  CHECK_FALSE(backend->setRate(0.0));
  CHECK(backend->state() == soar::PlaybackState::Playing);

  backend->stop();
  backend->close();
}

TEST_CASE("runtime audio switching continues playback seamlessly") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping audio switching test");
    return;
  }

  // sink declared first: it must outlive the backend.
  CountingSink sink;
  auto backend = soar::makeFFmpegBackend();
  backend->setEventSink(&sink);

  REQUIRE(backend->open(soar::MediaSource{media}));
  // The dual-audio sample maps stream 0 to video and 1/2 to audio.
  REQUIRE(backend->mediaInfo().selected_audio == 1);

  REQUIRE(backend->play());
  std::this_thread::sleep_for(300ms);
  const auto before_switch = backend->position();
  REQUIRE(before_switch > 0ms);

  // Switching the audio track while playing must not disturb the clock:
  // the decode thread hands over at a safe point and seeks back to the
  // current position.
  CHECK(backend->selectTrack(soar::TrackType::Audio, 2));
  bool switched = false;
  for (int i = 0; i < 500 && !switched; ++i) {
    switched = backend->mediaInfo().selected_audio == 2;
    if (!switched) {
      std::this_thread::sleep_for(10ms);
    }
  }
  CHECK(switched);
  CHECK(sink.errors.load() == 0);

  // Playback continues past the switch point (no reset, no freeze).
  bool advanced = false;
  for (int i = 0; i < 500 && !advanced; ++i) {
    advanced = backend->position() > before_switch;
    if (!advanced) {
      std::this_thread::sleep_for(10ms);
    }
  }
  CHECK(advanced);

  // The same handover works while paused, and keeps the paused state.
  CHECK(backend->pause());
  const auto frozen = backend->position();
  CHECK(backend->selectTrack(soar::TrackType::Audio, 1));
  bool switched_back = false;
  for (int i = 0; i < 500 && !switched_back; ++i) {
    switched_back = backend->mediaInfo().selected_audio == 1;
    if (!switched_back) {
      std::this_thread::sleep_for(10ms);
    }
  }
  CHECK(switched_back);
  CHECK(backend->state() == soar::PlaybackState::Paused);
  std::this_thread::sleep_for(100ms);
  CHECK(backend->position() == frozen);

  // And playback resumes from where it was.
  CHECK(backend->play());
  CHECK(backend->state() == soar::PlaybackState::Playing);

  backend->stop();
  backend->close();
  // Neither the playing switch, the paused switch, nor the resume
  // produced an error event.
  CHECK(sink.errors.load() == 0);
}

TEST_CASE("reopening without an explicit close restarts cleanly") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping reopen test");
    return;
  }

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{media}));
  REQUIRE(backend->play());
  std::this_thread::sleep_for(200ms);
  REQUIRE(backend->position() > 0ms);

  // open() closes any existing media first; the result is a fresh,
  // stopped session on the same backend instance.
  CHECK(backend->open(soar::MediaSource{media}));
  CHECK(backend->state() == soar::PlaybackState::Stopped);
  CHECK(backend->position() == 0ms);

  // The fresh session plays.
  CHECK(backend->play());
  CHECK(backend->state() == soar::PlaybackState::Playing);
  bool advanced = false;
  for (int i = 0; i < 200 && !advanced; ++i) {
    advanced = backend->position() > 0ms;
    if (!advanced) {
      std::this_thread::sleep_for(10ms);
    }
  }
  CHECK(advanced);

  backend->stop();
  backend->close();
}

TEST_CASE("selectTrack rejects invalid targets without disturbing playback") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping selectTrack rejection test");
    return;
  }

  // sink declared first: it must outlive the backend.
  CountingSink sink;
  auto backend = soar::makeFFmpegBackend();
  backend->setEventSink(&sink);

  REQUIRE(backend->open(soar::MediaSource{media}));
  REQUIRE(backend->play());

  CHECK_FALSE(backend->selectTrack(soar::TrackType::Video, 0));
  CHECK(backend->lastError().find("video") != std::string::npos);
  CHECK_FALSE(backend->selectTrack(soar::TrackType::Audio, 99));
  CHECK_FALSE(backend->selectTrack(soar::TrackType::Subtitle, 99));
  CHECK(sink.errors.load() >= 3);

  // The failed attempts leave playback untouched.
  CHECK(backend->state() == soar::PlaybackState::Playing);
  bool advanced = false;
  for (int i = 0; i < 200 && !advanced; ++i) {
    advanced = backend->position() > 0ms;
    if (!advanced) {
      std::this_thread::sleep_for(10ms);
    }
  }
  CHECK(advanced);

  backend->stop();
  backend->close();
}

TEST_CASE("event sink can be swapped and detached") {
  // sink declared first: it must outlive the backend.
  CountingSink sink_a;
  CountingSink sink_b;
  auto backend = soar::makeFFmpegBackend();

  backend->setEventSink(&sink_a);
  CHECK_FALSE(backend->play()); // error event -> sink_a
  CHECK(sink_a.errors.load() >= 1);
  CHECK(sink_b.errors.load() == 0);

  // Rebinding moves delivery to the new sink only.
  backend->setEventSink(&sink_b);
  CHECK_FALSE(backend->pause());
  CHECK(sink_b.errors.load() >= 1);
  const auto a_frozen = sink_a.errors.load();
  CHECK_FALSE(backend->stop());
  CHECK(sink_b.errors.load() > 1);
  CHECK(sink_a.errors.load() == a_frozen);

  // Detaching stops delivery entirely: a later failure still fails the
  // call, but no event reaches either sink.
  const auto b_before = sink_b.errors.load();
  backend->setEventSink(nullptr);
  CHECK_FALSE(backend->seek(100ms));
  CHECK(sink_b.errors.load() == b_before);
  CHECK(sink_a.errors.load() == a_frozen);
  CHECK(b_before >= 2);

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
