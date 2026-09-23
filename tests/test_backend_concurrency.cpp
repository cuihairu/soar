// Concurrency tests for FFmpegBackend.
//
// format_ctx_ must only be touched under decode_mutex_ (see the fix that
// introduced hasMedia() and the locked open/seek paths). The storms below
// hammer exactly those entry points from multiple threads so that a race
// detector (TSan in CI) or plain scheduler noise catches a regression.
//
// When SOAR_TEST_MEDIA points at a real media file, playback-time storms
// run as well; otherwise those cases report a message and return early.
// CI (Linux) generates the media with the ffmpeg CLI.

#ifdef SOAR_WITH_FFMPEG
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "soar/core/ffmpeg_backend.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

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

// Runs fn on `threads` threads concurrently and joins them.
template <typename Fn>
void runStorm(int threads, std::chrono::milliseconds duration, Fn&& fn) {
  std::atomic<bool> stop{false};
  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(threads));
  for (int i = 0; i < threads; ++i) {
    workers.emplace_back([&stop, &fn, i] {
      std::mt19937 rng(static_cast<unsigned>(i) + 1u);
      while (!stop.load(std::memory_order_relaxed)) {
        fn(rng);
      }
    });
  }
  std::this_thread::sleep_for(duration);
  stop.store(true);
  for (auto& t : workers) {
    t.join();
  }
}

} // namespace

TEST_CASE("unopened backend survives a control-call storm") {
  auto backend = soar::makeFFmpegBackend();

  runStorm(4, 150ms, [&backend](std::mt19937& rng) {
    switch (rng() % 6) {
      case 0: (void)backend->pause(); break;
      case 1: (void)backend->stop(); break;
      case 2: (void)backend->seek(100ms); break;
      case 3: (void)backend->state(); break;
      case 4: (void)backend->position(); break;
      default: (void)backend->lastError(); break;
    }
  });

  // The backend must still be usable and consistent afterwards.
  CHECK(backend->state() == soar::PlaybackState::Stopped);
  CHECK_FALSE(backend->open(soar::MediaSource{"/definitely/missing/file.mp4"}));
  CHECK_FALSE(backend->lastError().empty());
  CHECK(backend->mediaInfo().tracks.empty());
}

TEST_CASE("open/close storm against control calls is race-free") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping open/close storm");
    return;
  }

  auto backend = soar::makeFFmpegBackend();
  std::atomic<int> open_ok{0};
  std::atomic<int> open_failed{0};

  // Two threads fight over the media lifecycle while two more hammer the
  // control surface; before the decode_mutex_ fix, the unlocked
  // format_ctx_ reads in pause/stop/seek raced with the writer in open().
  std::atomic<bool> stop_flag{false};
  std::vector<std::thread> workers;
  workers.emplace_back([&] {
    while (!stop_flag.load()) {
      if (backend->open(soar::MediaSource{media})) {
        ++open_ok;
      } else {
        ++open_failed;
      }
    }
  });
  workers.emplace_back([&] {
    while (!stop_flag.load()) {
      backend->close();
    }
  });
  workers.emplace_back([&] {
    std::mt19937 rng(7);
    while (!stop_flag.load()) {
      switch (rng() % 3) {
        case 0: (void)backend->pause(); break;
        case 1: (void)backend->stop(); break;
        default: (void)backend->seek(50ms); break;
      }
    }
  });
  workers.emplace_back([&] {
    while (!stop_flag.load()) {
      (void)backend->position();
      (void)backend->state();
      (void)backend->mediaInfo();
    }
  });

  std::this_thread::sleep_for(300ms);
  stop_flag.store(true);
  for (auto& t : workers) {
    t.join();
  }

  CHECK(open_ok.load() + open_failed.load() > 0);
  backend->close();
  CHECK(backend->state() == soar::PlaybackState::Stopped);
}

TEST_CASE("playback-time control storm stays consistent") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping playback storm");
    return;
  }

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{media}));
  REQUIRE(backend->play());

  runStorm(4, 300ms, [&backend](std::mt19937& rng) {
    switch (rng() % 5) {
      case 0: (void)backend->pause(); break;
      case 1: (void)backend->play(); break;
      case 2: (void)backend->seek(std::chrono::milliseconds(rng() % 3000)); break;
      case 3: (void)backend->setRate(rng() % 2 ? 1.0 : 1.5); break;
      default: {
        const auto s = backend->state();
        const auto p = backend->position();
        (void)s;
        (void)p;
        break;
      }
    }
  });

  CHECK(backend->stop());
  backend->close();
  CHECK(backend->state() == soar::PlaybackState::Stopped);
  CHECK(backend->position() == 0ms);
}

namespace {

// Returns the audio (and one video) track ids of the test media.
struct TrackIds {
  std::vector<soar::TrackId> audio;
  soar::TrackId video{-1};
};

TrackIds collectTracks(const soar::MediaInfo& info) {
  TrackIds ids;
  for (const auto& t : info.tracks) {
    if (t.type == soar::TrackType::Audio) {
      ids.audio.push_back(t.id);
    } else if (t.type == soar::TrackType::Video) {
      ids.video = t.id;
    }
  }
  return ids;
}

// Waits until the decode loop has installed the requested audio track
// (the switch is applied asynchronously at a packet boundary).
bool waitSelectedAudio(soar::IBackend& backend, soar::TrackId id) {
  for (int i = 0; i < 200; ++i) {
    if (backend.mediaInfo().selected_audio == id) {
      return true;
    }
    std::this_thread::sleep_for(10ms);
  }
  return backend.mediaInfo().selected_audio == id;
}

} // namespace

TEST_CASE("runtime audio track switching while playing and paused") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping track-switching test");
    return;
  }

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{media}));
  const auto ids = collectTracks(backend->mediaInfo());
  REQUIRE(ids.audio.size() >= 2);

  // Unknown ids and video tracks stay rejected.
  CHECK_FALSE(backend->selectTrack(soar::TrackType::Audio, 9999));
  CHECK_FALSE(backend->selectTrack(soar::TrackType::Video, ids.video));

  // Switch while playing: applied asynchronously, playback continues.
  REQUIRE(backend->play());
  CHECK(backend->selectTrack(soar::TrackType::Audio, ids.audio[1]));
  CHECK(waitSelectedAudio(*backend, ids.audio[1]));
  CHECK(backend->state() == soar::PlaybackState::Playing);

  // Switch while paused: the decode loop applies it from its wait state.
  CHECK(backend->pause());
  CHECK(backend->selectTrack(soar::TrackType::Audio, ids.audio[0]));
  CHECK(waitSelectedAudio(*backend, ids.audio[0]));
  CHECK(backend->state() == soar::PlaybackState::Paused);
  CHECK(backend->play());

  // Switching to the track that is already selected succeeds as a no-op.
  CHECK(backend->selectTrack(soar::TrackType::Audio, ids.audio[0]));
  CHECK(waitSelectedAudio(*backend, ids.audio[0]));

  CHECK(backend->stop());
  backend->close();
  CHECK(backend->state() == soar::PlaybackState::Stopped);
}

TEST_CASE("track-switch storm against control calls is race-free") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping track-switch storm");
    return;
  }

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{media}));
  const auto ids = collectTracks(backend->mediaInfo());
  REQUIRE(ids.audio.size() >= 2);
  REQUIRE(backend->play());

  // One thread flips between the two audio tracks while the others hammer
  // the control surface; before the pending-decoder handoff this raced on
  // audio_decoder_ (and the error-string path aborted on macOS).
  runStorm(4, 300ms, [&backend, &ids](std::mt19937& rng) {
    switch (rng() % 5) {
      case 0: (void)backend->selectTrack(soar::TrackType::Audio, ids.audio[rng() % 2]); break;
      case 1: (void)backend->pause(); break;
      case 2: (void)backend->play(); break;
      case 3: (void)backend->seek(std::chrono::milliseconds(rng() % 3000)); break;
      default: {
        const auto s = backend->state();
        const auto i = backend->mediaInfo().selected_audio;
        (void)s;
        (void)i;
        break;
      }
    }
  });

  // The backend must still shut down cleanly after the storm.
  const auto final_state = backend->state();
  CHECK((final_state == soar::PlaybackState::Playing ||
         final_state == soar::PlaybackState::Paused ||
         final_state == soar::PlaybackState::Stopped));
  CHECK(backend->stop());
  backend->close();
  CHECK(backend->state() == soar::PlaybackState::Stopped);
  CHECK(backend->mediaInfo().tracks.empty());
}

#else // !SOAR_WITH_FFMPEG

// Keep the test binary meaningful when the FFmpeg backend is not compiled.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

TEST_CASE("ffmpeg backend concurrency tests require SOAR_WITH_FFMPEG") {
  MESSAGE("FFmpeg backend not compiled in; nothing to test here");
}

#endif
