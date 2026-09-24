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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace {

bool envMedia(const char* name, std::string& out_path) {
  const char* path = std::getenv(name);
  if (!path || !*path) {
    return false;
  }
  std::ifstream f(path);
  if (!f.good()) {
    return false;
  }
  out_path = path;
  return true;
}

bool mediaAvailable(std::string& out_path) {
  return envMedia("SOAR_TEST_MEDIA", out_path);
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

  // Seeking to the end again while already Ended keeps the state: the
  // Ended transition fires on the change, not on every seek.
  CHECK(backend->seek(duration));
  CHECK(backend->position() == duration);
  CHECK(backend->state() == soar::PlaybackState::Ended);

  // Seeking away from the end synchronously resumes to Paused, so the
  // play() below continues from the seek target instead of restarting.
  CHECK(backend->seek(0ms));
  CHECK(backend->position() == 0ms);
  CHECK(backend->state() == soar::PlaybackState::Paused);

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
  // Audio id 0 exists but is the video stream: a type/id mismatch.
  CHECK_FALSE(backend->selectTrack(soar::TrackType::Audio, 0));
  // Negative ids are rejected by the range guard, not wrapped around.
  CHECK_FALSE(backend->selectTrack(soar::TrackType::Audio, -1));
  CHECK_FALSE(backend->selectTrack(soar::TrackType::Subtitle, -1));
  CHECK(sink.errors.load() >= 6);

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

TEST_CASE("media info reports track layout and codecs") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping media info test");
    return;
  }

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{media}));

  const auto info = backend->mediaInfo();
  // The sample maps 0:v (ffvhuff), 1:a and 2:a (both pcm_s16le).
  REQUIRE(info.tracks.size() == 3);

  int videos = 0;
  int audios = 0;
  int subtitles = 0;
  for (const auto& t : info.tracks) {
    switch (t.type) {
      case soar::TrackType::Video: ++videos; break;
      case soar::TrackType::Audio: ++audios; break;
      case soar::TrackType::Subtitle: ++subtitles; break;
    }
  }
  CHECK(videos == 1);
  CHECK(audios == 2);
  CHECK(subtitles == 0);

  CHECK(info.tracks[0].codec == "ffvhuff");
  CHECK(info.tracks[1].codec == "pcm_s16le");
  CHECK(info.tracks[2].codec == "pcm_s16le");

  // Titles come from stream metadata; the video stream has none and
  // falls back to the type-based default.
  CHECK(info.tracks[0].title == "Video");
  CHECK(info.tracks[1].title == "Sine 440");
  CHECK(info.tracks[2].title == "Sine 880");

  // Generated with -t 6; allow encoder/rounding slack.
  CHECK(info.duration > std::chrono::milliseconds(5000));
  CHECK(info.duration < std::chrono::milliseconds(8000));

  backend->close();
}

TEST_CASE("tryGetVideoFrame delivers frames during playback") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping video frame test");
    return;
  }

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{media}));
  REQUIRE(backend->play());

  // The decode thread needs a moment to produce the first frame; the
  // wait also absorbs sanitizer-slowed runs.
  soar::FFmpegBackend::DecodedVideoFrame frame;
  bool got = false;
  for (int i = 0; i < 1000 && !got; ++i) {
    got = static_cast<soar::FFmpegBackend*>(backend.get())->tryGetVideoFrame(frame);
    if (!got) {
      std::this_thread::sleep_for(10ms);
    }
  }
  REQUIRE(got);

  // The sample is testsrc rendered at 160x120, exported as YUV420P.
  CHECK(frame.width == 160);
  CHECK(frame.height == 120);
  CHECK_FALSE(frame.y.empty());
  CHECK_FALSE(frame.u.empty());
  CHECK_FALSE(frame.v.empty());
  CHECK(frame.pts >= 0ms);

  backend->stop();
  backend->close();
}

TEST_CASE("media without subtitle tracks rejects subtitle selection") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping subtitle rejection test");
    return;
  }

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{media}));
  REQUIRE(backend->mediaInfo().selected_subtitle == -1);

  // Every stream id of the sample is video or audio, so subtitle
  // selection must fail for all of them.
  CHECK_FALSE(backend->selectTrack(soar::TrackType::Subtitle, 0));
  CHECK_FALSE(backend->selectTrack(soar::TrackType::Subtitle, 1));
  CHECK_FALSE(backend->selectTrack(soar::TrackType::Subtitle, 2));
  CHECK(backend->mediaInfo().selected_subtitle == -1);

  // Disabling subtitles is still a successful no-op.
  CHECK(backend->disableSubtitles());
  CHECK(backend->mediaInfo().selected_subtitle == -1);

  backend->close();
}

TEST_CASE("seeking while playing jumps to the target") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping live seek test");
    return;
  }

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{media}));
  REQUIRE(backend->play());
  std::this_thread::sleep_for(300ms);
  REQUIRE(backend->position() < 2000ms);

  // 5500ms cannot be reached by natural playback within the poll budget
  // (5s at rate 1x from a sub-second start), so reaching it proves the
  // seek took effect.
  CHECK(backend->seek(std::chrono::milliseconds(5500)));
  bool seeked = false;
  for (int i = 0; i < 500 && !seeked; ++i) {
    seeked = backend->position() >= std::chrono::milliseconds(5500);
    if (!seeked) {
      std::this_thread::sleep_for(10ms);
    }
  }
  CHECK(seeked);

  backend->stop();
  backend->close();
}

TEST_CASE("rapid audio switching converges on the final selection") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping rapid switching test");
    return;
  }

  // sink declared first: it must outlive the backend.
  CountingSink sink;
  auto backend = soar::makeFFmpegBackend();
  backend->setEventSink(&sink);

  REQUIRE(backend->open(soar::MediaSource{media}));
  REQUIRE(backend->play());
  std::this_thread::sleep_for(300ms);
  const auto before = backend->position();

  // Hammer the pending-slot handover: switches issued faster than the
  // decode loop consumes them must leave the final selection in charge.
  CHECK(backend->selectTrack(soar::TrackType::Audio, 2));
  CHECK(backend->selectTrack(soar::TrackType::Audio, 1));
  CHECK(backend->selectTrack(soar::TrackType::Audio, 2));
  CHECK(backend->selectTrack(soar::TrackType::Audio, 1));
  CHECK(backend->mediaInfo().selected_audio == 1);
  CHECK(backend->state() == soar::PlaybackState::Playing);

  // The decode loop settles on the final track and playback continues.
  bool advanced = false;
  for (int i = 0; i < 500 && !advanced; ++i) {
    advanced = backend->position() > before;
    if (!advanced) {
      std::this_thread::sleep_for(10ms);
    }
  }
  CHECK(advanced);
  CHECK(sink.errors.load() == 0);

  backend->stop();
  backend->close();
  CHECK(sink.errors.load() == 0);
}

TEST_CASE("backend recovers from a failed open") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping failed-open recovery test");
    return;
  }

  auto backend = soar::makeFFmpegBackend();

  CHECK_FALSE(backend->open(soar::MediaSource{"/definitely/missing/file.mp4"}));
  CHECK(backend->state() == soar::PlaybackState::Error);

  // The same instance must open and play real media afterwards.
  CHECK(backend->open(soar::MediaSource{media}));
  CHECK(backend->state() == soar::PlaybackState::Stopped);
  CHECK_FALSE(backend->mediaInfo().tracks.empty());
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

TEST_CASE("serial open/close cycles stay clean") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping open/close cycle test");
    return;
  }

  auto backend = soar::makeFFmpegBackend();
  for (int cycle = 0; cycle < 4; ++cycle) {
    INFO("cycle ", cycle);
    CHECK(backend->open(soar::MediaSource{media}));
    CHECK(backend->state() == soar::PlaybackState::Stopped);
    CHECK(backend->position() == 0ms);
    const auto duration = backend->mediaInfo().duration;
    CHECK(duration > 0ms);

    CHECK(backend->play());
    bool advanced = false;
    for (int i = 0; i < 200 && !advanced; ++i) {
      advanced = backend->position() > 0ms;
      if (!advanced) {
        std::this_thread::sleep_for(10ms);
      }
    }
    CHECK(advanced);

    CHECK(backend->stop());
    backend->close();
    CHECK(backend->mediaInfo().tracks.empty());
    CHECK(backend->position() == 0ms);
    CHECK(backend->state() == soar::PlaybackState::Stopped);
  }
}

TEST_CASE("stop is idempotent and play after stop restarts") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping stop idempotence test");
    return;
  }

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{media}));
  REQUIRE(backend->play());
  std::this_thread::sleep_for(250ms);
  REQUIRE(backend->position() > 0ms);

  CHECK(backend->stop());
  CHECK(backend->state() == soar::PlaybackState::Stopped);
  CHECK(backend->position() == 0ms);

  // Stopping an already-stopped backend is a success, not an error.
  CHECK(backend->stop());
  CHECK(backend->state() == soar::PlaybackState::Stopped);

  // And playback restarts cleanly from the beginning.
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

TEST_CASE("audio-only media drives position and seeks via the audio stream") {
  std::string media;
  if (!envMedia("SOAR_TEST_AUDIO_ONLY", media)) {
    MESSAGE("SOAR_TEST_AUDIO_ONLY not set; skipping audio-only test");
    return;
  }

  CountingSink sink;
  auto backend = soar::makeFFmpegBackend();
  backend->setEventSink(&sink);

  REQUIRE(backend->open(soar::MediaSource{media}));
  const auto info = backend->mediaInfo();
  REQUIRE(info.tracks.size() == 1);
  CHECK(info.tracks[0].type == soar::TrackType::Audio);
  CHECK(info.selected_video == -1);
  CHECK(info.selected_audio >= 0);
  CHECK(info.seekable);
  // Generated with -t 6; allow encoder/rounding slack.
  CHECK(info.duration > 5s);
  CHECK(info.duration < 8s);

  // With no video stream the position clock is driven by audio frames.
  REQUIRE(backend->play());
  std::this_thread::sleep_for(700ms);
  CHECK(backend->state() == soar::PlaybackState::Playing);
  CHECK(backend->position() > 200ms);
  CHECK(sink.position_changed.load() >= 1);

  // Seek resolves through the audio stream index (the decode thread is
  // still running, so the request is consumed at a packet boundary).
  REQUIRE(backend->seek(1s));
  bool landed = false;
  for (int i = 0; i < 300 && !landed; ++i) {
    landed = backend->position() >= 1s;
    if (!landed) {
      std::this_thread::sleep_for(10ms);
    }
  }
  CHECK(landed);
  CHECK(backend->position() < 2s);

  // A rate change rebuilds the resampler and the SDL device; position
  // keeps advancing afterwards (faster now).
  REQUIRE(backend->setRate(2.0));
  std::this_thread::sleep_for(400ms);
  CHECK(sink.position_changed.load() >= 2);

  // Volume attenuation and muting run the scale path without stalling.
  CHECK(backend->setVolume(0.5));
  std::this_thread::sleep_for(300ms);
  CHECK(backend->setMuted(true));
  std::this_thread::sleep_for(200ms);
  CHECK(backend->state() == soar::PlaybackState::Playing);

  CHECK(backend->stop());
  backend->close();
}

TEST_CASE("natural EOF keeps the thread joinable for seek and track switch") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping natural EOF test");
    return;
  }

  CountingSink sink;
  auto backend = soar::makeFFmpegBackend();
  backend->setEventSink(&sink);

  REQUIRE(backend->open(soar::MediaSource{media}));
  REQUIRE(backend->play());

  // 6s media; wait up to 12s for the natural end (sanitizer-slow runs).
  auto waitEnded = [&] {
    for (int i = 0; i < 120; ++i) {
      if (backend->state() == soar::PlaybackState::Ended) {
        return true;
      }
      std::this_thread::sleep_for(100ms);
    }
    return backend->state() == soar::PlaybackState::Ended;
  };
  REQUIRE(waitEnded());

  // After EOF the decode thread has exited but is still joinable; seek()
  // joins it and applies the seek synchronously, resuming paused.
  REQUIRE(backend->seek(1s));
  CHECK(backend->state() == soar::PlaybackState::Paused);
  CHECK(backend->position() == 1s);

  // Play to the end once more, then switch tracks: the exited thread is
  // joined and the new decoder is swapped in directly, leaving the state
  // at Stopped (nothing is playing anymore).
  REQUIRE(backend->play());
  REQUIRE(waitEnded());

  const auto info = backend->mediaInfo();
  int other_audio = -1;
  for (const auto& t : info.tracks) {
    if (t.type == soar::TrackType::Audio && t.id != info.selected_audio) {
      other_audio = t.id;
    }
  }
  REQUIRE(other_audio >= 0);
  REQUIRE(backend->selectTrack(soar::TrackType::Audio, other_audio));
  CHECK(backend->state() == soar::PlaybackState::Stopped);
  CHECK(backend->mediaInfo().selected_audio == other_audio);
  CHECK(backend->lastError().empty());

  backend->close();
}

TEST_CASE("stop after natural EOF joins the finished thread and resets") {
  std::string media;
  if (!envMedia("SOAR_TEST_AUDIO_ONLY", media)) {
    MESSAGE("SOAR_TEST_AUDIO_ONLY not set; skipping EOF stop test");
    return;
  }

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{media}));
  REQUIRE(backend->play());

  bool ended = false;
  for (int i = 0; i < 120 && !ended; ++i) {
    ended = backend->state() == soar::PlaybackState::Ended;
    if (!ended) {
      std::this_thread::sleep_for(100ms);
    }
  }
  REQUIRE(ended);

  // stop() while the decode thread has already run out of data at EOF:
  // the thread is not joinable-active but is still joinable, and stop
  // must reap it and reset to Stopped without hanging or erroring.
  CHECK(backend->stop());
  CHECK(backend->state() == soar::PlaybackState::Stopped);
  CHECK(backend->position() == 0ms);
  CHECK(backend->lastError().empty());

  // Reopening proves the reap left no residue behind.
  REQUIRE(backend->open(soar::MediaSource{media}));
  CHECK(backend->state() == soar::PlaybackState::Stopped);
  CHECK(backend->mediaInfo().duration > 0ms);

  backend->close();
}

TEST_CASE("closing without ever playing leaves nothing to join") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping unplayed close test");
    return;
  }

  {
    auto backend = soar::makeFFmpegBackend();
    REQUIRE(backend->open(soar::MediaSource{media}));
    CHECK(backend->state() == soar::PlaybackState::Stopped);

    // Straight to close() without play(): the decode thread was never
    // started, so close() must skip the join path entirely.
    backend->close();
    CHECK(backend->state() == soar::PlaybackState::Stopped);
    CHECK(backend->mediaInfo().tracks.empty());
  }
}

TEST_CASE("pause and repeated play are safe outside the playing state") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping pause/replay test");
    return;
  }

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{media}));

  // Pausing before anything plays: pause() unconditionally enters the
  // Paused state (only an explicit stop() returns to Stopped), and the
  // clock starts from position 0 once play() runs.
  CHECK(backend->pause());
  CHECK(backend->state() == soar::PlaybackState::Paused);
  CHECK(backend->position() == 0ms);

  // A repeated play() while already playing stays playing and keeps the
  // position moving instead of restarting the media.
  REQUIRE(backend->play());
  std::this_thread::sleep_for(200ms);
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

  // Pause while the decode thread is still running, then let the scope
  // end: the destructor must take the paused-backend shutdown path
  // (join the alive thread, no playing clock) just as cleanly.
  CHECK(backend->pause());
  CHECK(backend->state() == soar::PlaybackState::Paused);
}

TEST_CASE("subtitle and attachment streams enumerate; subtitles select") {
  std::string media;
  if (!envMedia("SOAR_TEST_SUBS_MEDIA", media)) {
    MESSAGE("SOAR_TEST_SUBS_MEDIA not set; skipping subtitle stream test");
    return;
  }

  CountingSink sink;
  auto backend = soar::makeFFmpegBackend();
  backend->setEventSink(&sink);

  REQUIRE(backend->open(soar::MediaSource{media}));
  const auto info = backend->mediaInfo();
  // video + audio + subtitle are enumerated; the container attachment is
  // not a track.
  REQUIRE(info.tracks.size() == 3);

  const soar::TrackInfo* sub = nullptr;
  for (const auto& t : info.tracks) {
    if (t.type == soar::TrackType::Subtitle) {
      sub = &t;
    }
  }
  REQUIRE(sub != nullptr);
  CHECK(sub->language == "eng");
  CHECK(info.selected_subtitle == -1); // subtitles default to off

  REQUIRE(backend->selectTrack(soar::TrackType::Subtitle, sub->id));
  CHECK(backend->mediaInfo().selected_subtitle == sub->id);
  CHECK(sink.media_info_changed.load() >= 1);

  CHECK(backend->disableSubtitles());
  CHECK(backend->mediaInfo().selected_subtitle == -1);

  // Disabling again with nothing selected stays a metadata-only success
  // that re-announces the (unchanged) info.
  const auto announcements = sink.media_info_changed.load();
  CHECK(backend->disableSubtitles());
  CHECK(backend->mediaInfo().selected_subtitle == -1);
  CHECK(sink.media_info_changed.load() >= announcements + 1);

  backend->close();
  // A second close() with nothing open or running must be a no-op.
  backend->close();
  CHECK(backend->state() == soar::PlaybackState::Stopped);
}

TEST_CASE("a container with no audio or video streams fails to open") {
  std::string subs_only;
  if (!envMedia("SOAR_TEST_SUBS_ONLY", subs_only)) {
    MESSAGE("SOAR_TEST_SUBS_ONLY not set; skipping no-stream test");
    return;
  }

  CountingSink sink;
  auto backend = soar::makeFFmpegBackend();
  backend->setEventSink(&sink);

  CHECK_FALSE(backend->open(soar::MediaSource{subs_only}));
  CHECK(backend->lastError().find("no video or audio stream") != std::string::npos);
  CHECK(backend->state() == soar::PlaybackState::Error);
  CHECK(backend->mediaInfo().tracks.empty());
  CHECK(sink.errors.load() >= 1);

  // The backend stays usable and opens real media afterwards.
  std::string media;
  REQUIRE(mediaAvailable(media));
  REQUIRE(backend->open(soar::MediaSource{media}));
  CHECK(backend->state() == soar::PlaybackState::Stopped);
  backend->close();
}

TEST_CASE("a video codec with no FFmpeg decoder fails to open") {
  std::string unknown;
  if (!envMedia("SOAR_TEST_UNKNOWN_CODEC", unknown)) {
    MESSAGE("SOAR_TEST_UNKNOWN_CODEC not set; skipping unknown-codec test");
    return;
  }

  CountingSink sink;
  auto backend = soar::makeFFmpegBackend();
  backend->setEventSink(&sink);

  // The container opens and enumerates the video stream, but no decoder
  // exists for its codec id: the open path must fail loudly instead of
  // handing out a playable-looking MediaInfo.
  CHECK_FALSE(backend->open(soar::MediaSource{unknown}));
  CHECK(backend->lastError().find("video codec not found") != std::string::npos);
  CHECK(backend->state() == soar::PlaybackState::Error);
  CHECK(sink.errors.load() >= 1);

  // The backend recovers and opens healthy media afterwards.
  std::string media;
  REQUIRE(mediaAvailable(media));
  REQUIRE(backend->open(soar::MediaSource{media}));
  CHECK(backend->state() == soar::PlaybackState::Stopped);
  backend->close();
}

TEST_CASE("undecodable video payload ends deterministically without hanging") {
  std::string corrupt;
  if (!envMedia("SOAR_TEST_CORRUPT_DECODE", corrupt)) {
    MESSAGE("SOAR_TEST_CORRUPT_DECODE not set; skipping corrupt-decode test");
    return;
  }

  CountingSink sink;
  auto backend = soar::makeFFmpegBackend();
  backend->setEventSink(&sink);

  // The container and the (mislabelled) decoder both open fine; the
  // h264 packets are then either rejected at send time (the decode loop
  // skips them and runs to natural EOF) or rejected per frame at
  // receive time (a fatal decode error). Both are legitimate behaviors
  // across FFmpeg versions; what must hold either way is that playback
  // ends deterministically - never a hang - and the backend recovers.
  REQUIRE(backend->open(soar::MediaSource{corrupt}));
  REQUIRE(backend->play());

  // 3s media; wait up to 12s for a terminal state (sanitizer-slow runs).
  bool finished = false;
  for (int i = 0; i < 120 && !finished; ++i) {
    const auto state = backend->state();
    finished = state == soar::PlaybackState::Ended ||
               state == soar::PlaybackState::Error;
    if (!finished) {
      std::this_thread::sleep_for(100ms);
    }
  }
  CHECK(finished);

  // Stopping from either terminal state is safe and leaves a reusable
  // backend.
  CHECK(backend->stop());
  CHECK(backend->state() == soar::PlaybackState::Stopped);
  std::string media;
  REQUIRE(mediaAvailable(media));
  REQUIRE(backend->open(soar::MediaSource{media}));
  CHECK(backend->state() == soar::PlaybackState::Stopped);
  backend->close();
}

TEST_CASE("truncated containers fail the open path at the right stage") {
  std::string tiny, mid;
  if (!envMedia("SOAR_TEST_TRUNCATED_TINY", tiny) ||
      !envMedia("SOAR_TEST_TRUNCATED_MID", mid)) {
    MESSAGE("SOAR_TEST_TRUNCATED_* not set; skipping truncated-container test");
    return;
  }

  CountingSink sink;
  auto backend = soar::makeFFmpegBackend();
  backend->setEventSink(&sink);

  // 64 bytes: not even the Matroska segment header survives, so the
  // demuxer itself refuses the input.
  CHECK_FALSE(backend->open(soar::MediaSource{tiny}));
  CHECK(backend->lastError().find("failed to open") != std::string::npos);
  CHECK(backend->state() == soar::PlaybackState::Error);

  // 512 bytes: header and track entries parse, but no frame can be read
  // to identify the codecs, so find_stream_info gives up instead.
  CHECK_FALSE(backend->open(soar::MediaSource{mid}));
  CHECK(backend->lastError().find("findStreamInfo") != std::string::npos);
  CHECK(backend->state() == soar::PlaybackState::Error);
  // open() reports each failure through the event sink itself.
  CHECK(sink.errors.load() >= 2);

  // The backend recovers and opens healthy media afterwards.
  std::string media;
  REQUIRE(mediaAvailable(media));
  REQUIRE(backend->open(soar::MediaSource{media}));
  CHECK(backend->state() == soar::PlaybackState::Stopped);
  backend->close();
}

TEST_CASE("destroying the backend while playing stops the decode thread") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping destroy-while-playing test");
    return;
  }

  {
    // sink declared first: it must outlive the backend destructor.
    CountingSink sink;
    auto backend = soar::makeFFmpegBackend();
    backend->setEventSink(&sink);

    REQUIRE(backend->open(soar::MediaSource{media}));
    REQUIRE(backend->play());
    std::this_thread::sleep_for(300ms);
    CHECK(backend->state() == soar::PlaybackState::Playing);
  } // destructor must join the live decode thread without hanging
}

TEST_CASE("a mid-stream resolution change rebuilds the video converter") {
  std::string media;
  if (!envMedia("SOAR_TEST_MULTI_RES", media)) {
    MESSAGE("SOAR_TEST_MULTI_RES not set; skipping resolution change test");
    return;
  }

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{media}));
  REQUIRE(backend->play());

  // The first half is 160x120; the second half is 320x240 (the h264
  // stream swaps SPS mid-flight). The converter must rebuild for the
  // new geometry instead of stalling or handing out stale-size frames.
  auto* ff = static_cast<soar::FFmpegBackend*>(backend.get());
  soar::FFmpegBackend::DecodedVideoFrame frame;
  bool saw_first = false;
  bool saw_second = false;
  for (int i = 0; i < 800 && !saw_second; ++i) {
    if (ff->tryGetVideoFrame(frame)) {
      saw_first = saw_first || frame.width == 160;
      saw_second = frame.width == 320 && frame.height == 240;
    }
    std::this_thread::sleep_for(10ms);
  }
  CHECK(saw_first);
  CHECK(saw_second);

  backend->stop();
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
