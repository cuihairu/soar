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

#include "soar/core/audio_extract.h"
#include "soar/core/ffmpeg_backend.h"
#include "soar/core/subtitle_provider.h"
#include "test_http_servers.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

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

// Position-based wait instead of a fixed sleep: on a cold page cache the
// first frame can take seconds to arrive, and a sleep long enough for the
// slow case only makes the fast case slower.
bool waitForPosition(const soar::IBackend& backend, std::chrono::milliseconds min_pos,
                     std::chrono::milliseconds budget) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    if (backend.position() >= min_pos) {
      return true;
    }
    std::this_thread::sleep_for(20ms);
  }
  return backend.position() >= min_pos;
}

struct CountingSink : soar::IEventSink {
  std::atomic<int> state_changed{0};
  std::atomic<int> media_info_changed{0};
  std::atomic<int> position_changed{0};
  std::atomic<int> errors{0};
  std::atomic<int> buffering_started{0};
  std::atomic<int> buffering_ended{0};

  void onEvent(const soar::Event& e) override {
    switch (e.type) {
      case soar::EventType::StateChanged: ++state_changed; break;
      case soar::EventType::MediaInfoChanged: ++media_info_changed; break;
      case soar::EventType::PositionChanged: ++position_changed; break;
      case soar::EventType::Error: ++errors; break;
      case soar::EventType::BufferingStarted: ++buffering_started; break;
      case soar::EventType::BufferingEnded: ++buffering_ended; break;
    }
  }
};

// Scratch directory for the external-subtitle cases. The media fixture is
// copied in under a chosen name so the sidecar beside it is a *different*
// file from the container's own embedded subtitle stream, and so the
// provider's name matching is exercised end to end rather than assumed.
struct ScratchDir {
  std::string path;

  ScratchDir() {
    std::string tmpl = "/tmp/soar_extsub_test_XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    const char* dir = ::mkdtemp(buf.data());
    path = dir ? std::string(dir) : std::string(".");
  }

  ~ScratchDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }

  std::string file(const std::string& name) const {
    return (std::filesystem::path(path) / name).string();
  }

  void write(const std::string& name, const std::string& content) const {
    std::ofstream out(file(name), std::ios::binary);
    out << content;
  }

  // Copy the fixture in under `name`. Returns the new path, or "" when the
  // copy failed (a full or read-only /tmp would otherwise look like a
  // backend bug three assertions later).
  std::string copyIn(const std::string& src, const std::string& name) const {
    std::error_code ec;
    std::filesystem::copy_file(
      src, file(name), std::filesystem::copy_options::overwrite_existing, ec);
    return ec ? std::string() : file(name);
  }
};

// Pull subtitle frames until `count` of them have been collected or the
// budget runs out. The playhead is what advances a sidecar (there is no
// decode thread behind one), so these cases cannot be tested without
// actually playing.
std::vector<soar::DecodedSubtitleFrame> pullSubtitleFrames(
    soar::FFmpegBackend* ffmpeg, std::size_t count, std::chrono::milliseconds budget) {
  std::vector<soar::DecodedSubtitleFrame> frames;
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline && frames.size() < count) {
    soar::DecodedSubtitleFrame frame;
    while (ffmpeg->tryGetSubtitleFrame(frame)) {
      frames.push_back(frame);
    }
    std::this_thread::sleep_for(5ms);
  }
  return frames;
}

bool contains(const std::vector<soar::DecodedSubtitleFrame>& frames,
              const std::string& needle) {
  for (const auto& f : frames) {
    if (f.text.find(needle) != std::string::npos) return true;
  }
  return false;
}

// --- WAV shape reader for the extraction battery -------------------------
// The extractor owns a format contract (16 kHz mono s16le) that ASR callers
// depend on, so the assertions parse the produced header instead of just
// checking the file is non-empty: a resampler that silently kept the source
// rate would still write a plausible-looking file.
struct WavShape {
  bool ok = false;
  std::string error;  // why ok is false, for assertion messages
  int audio_format = 0;
  int channels = 0;
  int sample_rate = 0;
  int bits = 0;
  long long data_bytes = 0;
};

// RIFF keeps chunk sizes and the fmt fields as binary little-endian
// integers, so they must be decoded byte-wise. Handing the raw bytes to
// strtoul reads them as decimal *text*: a size byte such as 0x10 is not a
// digit, so every field parses as 0 and the chunk walk runs off the end.
unsigned readLe(const std::string& blob, std::size_t off, std::size_t width) {
  unsigned value = 0;
  for (std::size_t i = 0; i < width && off + i < blob.size(); ++i) {
    value |= static_cast<unsigned>(static_cast<unsigned char>(blob[off + i]))
             << (8 * i);
  }
  return value;
}

WavShape readWavShape(const std::string& path) {
  WavShape s;
  std::ifstream f(path, std::ios::binary);
  if (!f.good()) {
    s.error = "cannot open " + path;
    return s;
  }
  std::string blob((std::istreambuf_iterator<char>(f)),
                   std::istreambuf_iterator<char>());
  if (blob.size() < 12) {
    s.error = "file is " + std::to_string(blob.size()) + " bytes, too short for RIFF/WAVE";
    return s;
  }
  if (blob.compare(0, 4, "RIFF") != 0 || blob.compare(8, 4, "WAVE") != 0) {
    s.error = "missing RIFF/WAVE magic";
    return s;
  }
  // Walk the chunk list rather than assuming the canonical 44-byte layout:
  // the wav muxer inserts a LIST/INFO chunk, so a test that hardcoded the
  // 44-byte offsets would read the tag bytes as format fields.
  std::size_t pos = 12;
  while (pos + 8 <= blob.size()) {
    const std::string id = blob.substr(pos, 4);
    const std::size_t body = pos + 8;
    if (id == "fmt " && body + 16 <= blob.size()) {
      s.audio_format = static_cast<int>(readLe(blob, body, 2));
      s.channels = static_cast<int>(readLe(blob, body + 2, 2));
      s.sample_rate = static_cast<int>(readLe(blob, body + 4, 4));
      s.bits = static_cast<int>(readLe(blob, body + 14, 2));
    } else if (id == "data") {
      s.data_bytes = static_cast<long long>(readLe(blob, pos + 4, 4));
      s.ok = true;
      return s;
    }
    const std::size_t size = readLe(blob, pos + 4, 4);
    pos = body + size + (size & 1);  // RIFF chunks are word aligned
  }
  s.error = "no data chunk among " + std::to_string(blob.size()) + " bytes";
  return s;
}

} // namespace

TEST_CASE("audio device enumeration cold-starts the SDL audio subsystem") {
  // Registered first in this TU so it runs before any media open has
  // touched SDL audio: SDL_INIT_AUDIO is still down and enumeration
  // itself brings the subsystem up. Every later caller in this process
  // (the device menu, the audio tests below) finds it already warm.
  CountingSink sink;
  auto backend = soar::makeFFmpegBackend();
  backend->setEventSink(&sink);

  CHECK(backend->currentAudioOutputDevice().empty());
  const auto devices = backend->audioOutputDevices();
  // The dummy driver reports one device; a runner without any audio
  // stack may report none — never assert the count, only that whatever
  // is listed is selectable and sticks.
  for (const auto& name : devices) {
    INFO("device: ", name);
    CHECK(backend->selectAudioOutputDevice(name));
    CHECK(backend->currentAudioOutputDevice() == name);
  }
  CHECK(backend->selectAudioOutputDevice(""));
  CHECK(backend->currentAudioOutputDevice().empty());
  CHECK(sink.errors.load() == 0);
}

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
  CHECK_FALSE(backend->setLoopAB(0ms, 1000ms));
  CHECK_FALSE(backend->clearLoopAB());
  CHECK(backend->lastError().empty() == false);
  CHECK(sink.errors.load() >= 9);

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
  std::chrono::milliseconds loop_a{0}, loop_b{0};
  CHECK_FALSE(backend->loopAB(loop_a, loop_b));
  // tryGetVideoFrame is an FFmpegBackend extension beyond IBackend.
  soar::DecodedVideoFrame frame;
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

  // The playback clock runs while playing. Under sanitizers the first
  // decoded frame can take well over 300 ms, so poll for the clock (and the
  // first PositionChanged, emitted at a 200 ms granularity) instead of
  // betting on a fixed sleep — a hung clock now fails on the same checks,
  // just later (docs/coverage-notes.md §3.5: never bet on wall clocks).
  bool clock_started = false;
  for (int i = 0; i < 1000 && !clock_started; ++i) {
    clock_started = backend->position() > 0ms && sink.position_changed.load() >= 1;
    if (!clock_started) {
      std::this_thread::sleep_for(10ms);
    }
  }
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

namespace {

// SOAR_TEST_HWACCEL_ARM scopes: the backend's test-only hwaccel arm seam
// (ffmpeg_backend.cpp, the saveScreenshot forceFailEncoder precedent). Runs
// headless CI through the armed-decoder path — get_format negotiation, the
// hardware-frame download branch, teardown — with real libavcodec. Restored
// on exit so later cases are unaffected.
struct ScopedArm {
  explicit ScopedArm(const char* value) {
    ::setenv("SOAR_TEST_HWACCEL_ARM", value, /*overwrite=*/1);
  }
  ~ScopedArm() { ::unsetenv("SOAR_TEST_HWACCEL_ARM"); }
};

}  // namespace

TEST_CASE("armed hwaccel decode runs the hardware-frame path and closes clean") {
  // SOAR_TEST_HWACCEL_ARM=1 stands in for the device create: the decoder
  // arms with a device reference on its own first software pixel format,
  // libavcodec runs the real get_format negotiation, and decodeLoop takes
  // the hardware-frame download branch — where the transfer correctly
  // fails (a software frame has no hw frames context) and the frame is
  // dropped. Only the transfer itself needs real hardware (docs/mvp.md
  // §3). The clock and audio stream run normally, so the playback clock
  // below proves the decode loop is live.
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping hwaccel arm test");
    return;
  }

  ScopedArm arm("1");
  CountingSink sink;
  auto backend = soar::makeFFmpegBackend();
  backend->setEventSink(&sink);

  REQUIRE(backend->open(soar::MediaSource{media, /*cache_dir=*/"", /*hwdec=*/"auto"}));
  REQUIRE(backend->play());

  // The download drops every video frame (the transfer needs a real hw
  // frames context), and the drop skips the presentation throttle — so
  // the decode thread drains the whole stream within milliseconds and lands
  // in Ended. Ended is the deterministic signal that the loop ran through
  // every video packet (and the download branch with them).
  bool drained = false;
  for (int i = 0; i < 1000 && !drained; ++i) {
    drained = backend->state() == soar::PlaybackState::Ended;
    if (!drained) {
      std::this_thread::sleep_for(10ms);
    }
  }
  CHECK(backend->state() == soar::PlaybackState::Ended);

  CHECK(backend->stop());
  backend->close();
  CHECK(backend->mediaInfo().tracks.empty());
}

TEST_CASE("an unknown hwdec name opens the source in software") {
  // Nothing validates hwdec below the CLI, so a typo reaching the backend
  // must behave as the documented "no hwaccel" fallback: an empty device
  // list leaves the decoder plain software.
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping unknown hwdec test");
    return;
  }

  CountingSink sink;
  auto backend = soar::makeFFmpegBackend();
  backend->setEventSink(&sink);

  REQUIRE(backend->open(soar::MediaSource{media, /*cache_dir=*/"", /*hwdec=*/"bogus"}));
  REQUIRE(backend->play());

  bool clock_started = false;
  for (int i = 0; i < 1000 && !clock_started; ++i) {
    clock_started = backend->position() > 0ms;
    if (!clock_started) {
      std::this_thread::sleep_for(10ms);
    }
  }
  CHECK(backend->position() > 0ms);

  CHECK(backend->stop());
  backend->close();
  CHECK(backend->mediaInfo().tracks.empty());
}

TEST_CASE("subtitle selection state and disableSubtitles work while stopped") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping subtitle metadata test");
    return;
  }

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{media}));

  CHECK(backend->mediaInfo().selected_subtitle == -1);

  // This fixture has no subtitle streams, so this stays the stopped-
  // state API surface: picking a real subtitle track (if a fixture ever
  // grows one) and disabling return to the off state. The decode-side
  // semantics of both — the gate selectTrack/disableSubtitles drive —
  // are covered by the dedicated selection cases.
  const auto tracks = backend->mediaInfo().tracks;
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

TEST_CASE("an armed A-B loop wraps playback back to point A") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping A-B loop test");
    return;
  }

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{media}));
  REQUIRE(backend->play());

  // Invalid windows are rejected up front and leave the previous arming
  // (here: none) untouched.
  std::chrono::milliseconds a{0}, b{0};
  CHECK_FALSE(backend->setLoopAB(2000ms, 1000ms));   // A >= B
  CHECK_FALSE(backend->setLoopAB(1000ms, 1000ms));   // empty window
  CHECK_FALSE(backend->setLoopAB(-100ms, 1000ms));   // A below zero
  CHECK_FALSE(backend->setLoopAB(1000ms, 7000ms));   // B past the 6s end
  CHECK_FALSE(backend->loopAB(a, b));

  // Arm a 1s..2s window and watch the play clock cross B and reappear
  // back inside [A, B): the wrap routes through the decode loop's seek
  // machinery, so the position re-converges on A like any seek. The
  // crossed_b guard keeps the initial 0 -> A ramp from faking a wrap.
  REQUIRE(backend->setLoopAB(1000ms, 2000ms));
  REQUIRE(backend->loopAB(a, b));
  CHECK(a == 1000ms);
  CHECK(b == 2000ms);

  bool crossed_b = false;
  bool wrapped = false;
  for (int i = 0; i < 300 && !(crossed_b && wrapped); ++i) {
    const auto p = backend->position();
    if (p >= 1900ms) crossed_b = true;
    if (crossed_b && p < 1200ms) wrapped = true;
    std::this_thread::sleep_for(50ms);
  }
  CHECK(crossed_b);
  CHECK(wrapped);
  CHECK(backend->state() == soar::PlaybackState::Playing);

  // A manual seek disarms the loop (mpv semantics): after seeking past B
  // the clock keeps climbing instead of being yanked back to A.
  CHECK(backend->seek(3000ms));
  CHECK_FALSE(backend->loopAB(a, b));
  bool seeked = false;
  for (int i = 0; i < 200 && !seeked; ++i) {
    seeked = backend->position() >= 3000ms;
    if (!seeked) {
      std::this_thread::sleep_for(10ms);
    }
  }
  CHECK(seeked);
  std::this_thread::sleep_for(300ms);
  CHECK(backend->position() >= 3000ms);
  CHECK(backend->state() == soar::PlaybackState::Playing);

  // stop() keeps a window armed (re-arm here: the seek above disarmed the
  // original); close() tears it down with everything else.
  CHECK(backend->setLoopAB(500ms, 1500ms));
  CHECK(backend->stop());
  CHECK(backend->loopAB(a, b));
  CHECK(a == 500ms);
  CHECK(b == 1500ms);
  backend->close();
  CHECK_FALSE(backend->loopAB(a, b));
}

TEST_CASE("an end-anchored A-B loop wraps at end-of-stream instead of ending") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping end-anchored A-B loop test");
    return;
  }

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{media}));

  // At 2x the 6s media reaches its end in ~3s of wall time; the window
  // [4s, 6s] anchors the media end, so EOF must rewind to A rather than
  // transition to Ended.
  CHECK(backend->setRate(2.0));
  REQUIRE(backend->play());
  REQUIRE(backend->setLoopAB(4000ms, 6000ms));

  bool neared_end = false;
  bool wrapped = false;
  for (int i = 0; i < 300 && !wrapped; ++i) {
    if (backend->state() == soar::PlaybackState::Ended) {
      break;
    }
    const auto p = backend->position();
    if (p >= 5000ms) neared_end = true;
    if (neared_end && p < 4500ms) wrapped = true;
    std::this_thread::sleep_for(50ms);
  }
  CHECK(neared_end);
  CHECK(wrapped);
  CHECK(backend->state() == soar::PlaybackState::Playing);

  // Clearing the loop restores the natural end: the stream runs out and
  // lands in Ended.
  CHECK(backend->clearLoopAB());
  bool ended = false;
  for (int i = 0; i < 300 && !ended; ++i) {
    ended = backend->state() == soar::PlaybackState::Ended;
    if (!ended) {
      std::this_thread::sleep_for(50ms);
    }
  }
  CHECK(ended);

  backend->close();
}

TEST_CASE("an end-anchored A-B loop wraps when the demuxer hits EOF before the clock") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping EOF-wrap A-B loop test");
    return;
  }

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{media}));

  // The 2x twin above wraps through the per-packet check: the clock runs
  // past B while packets are still arriving. This case exercises the other
  // anchor of the same contract — the demuxer runs out while the clock is
  // still short of B, so it is the EOF branch that must route the wrap
  // back to A instead of flipping to Ended. Half rate widens that margin
  // (decode-time debt counts against the clock at half weight), and the
  // pre-seed seek keeps the whole case a few seconds long.
  CHECK(backend->setRate(0.5));
  REQUIRE(backend->play());
  REQUIRE(backend->seek(5000ms));
  REQUIRE(waitForPosition(*backend, 4900ms, 30s));
  REQUIRE(backend->setLoopAB(4500ms, 6000ms));

  bool neared_end = false;
  bool wrapped = false;
  for (int i = 0; i < 600 && !wrapped; ++i) {
    if (backend->state() == soar::PlaybackState::Ended) {
      break;
    }
    const auto p = backend->position();
    if (p >= 5300ms) neared_end = true;
    if (neared_end && p <= 4700ms) wrapped = true;
    std::this_thread::sleep_for(20ms);
  }
  CHECK(neared_end);
  CHECK(wrapped);
  CHECK(backend->state() == soar::PlaybackState::Playing);

  // The wrap serves the loop, it must not disarm it.
  std::chrono::milliseconds a{0}, b{0};
  CHECK(backend->loopAB(a, b));
  CHECK(a == 4500ms);
  CHECK(b == 6000ms);

  // Clearing restores the natural end from wherever the wrap left us.
  CHECK(backend->clearLoopAB());
  bool ended = false;
  for (int i = 0; i < 400 && !ended; ++i) {
    ended = backend->state() == soar::PlaybackState::Ended;
    if (!ended) {
      std::this_thread::sleep_for(50ms);
    }
  }
  CHECK(ended);

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
  // Poll for the playback clock (the decode thread starts at its own
  // pace; a flat sleep bets on an idle machine and loses under load).
  bool started = false;
  for (int i = 0; i < 200 && !started; ++i) {
    started = backend->position() > 0ms;
    if (!started) {
      std::this_thread::sleep_for(10ms);
    }
  }
  REQUIRE(started);

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
  soar::DecodedVideoFrame frame;
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

TEST_CASE("saveScreenshot encodes the presented frame as PNG") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping screenshot test");
    return;
  }

  auto backend = soar::makeFFmpegBackend();
  auto* ffmpeg = static_cast<soar::FFmpegBackend*>(backend.get());
  const std::string out = "soar_backend_screenshot.png";
  ::unlink(out.c_str());

  // Before any frame is presented the call fails, records the reason in
  // lastError(), and writes nothing.
  CHECK_FALSE(ffmpeg->saveScreenshot(out));
  CHECK_FALSE(ffmpeg->lastError().empty());
  CHECK(::access(out.c_str(), F_OK) != 0);

  REQUIRE(backend->open(soar::MediaSource{media}));
  REQUIRE(backend->play());

  // The decode thread presents frames; the first call(s) may still land
  // before one has been retained, so retry like the UI does on a keypress.
  bool saved = false;
  for (int i = 0; i < 1000 && !saved; ++i) {
    saved = ffmpeg->saveScreenshot(out);
    if (!saved) {
      std::this_thread::sleep_for(10ms);
    }
  }
  REQUIRE(saved);

  // Validate the PNG container: signature, IHDR dimensions (the sample
  // is testsrc rendered at 160x120), and a plausible payload size.
  std::ifstream in(out, std::ios::binary);
  REQUIRE(in.good());
  std::vector<unsigned char> bytes(
    (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  REQUIRE(bytes.size() > 32);
  const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  CHECK(std::equal(bytes.begin(), bytes.begin() + 8, sig));
  CHECK(bytes[12] == 'I');
  CHECK(bytes[13] == 'H');
  CHECK(bytes[14] == 'D');
  CHECK(bytes[15] == 'R');
  const auto be32 = [&](std::size_t off) {
    return (static_cast<unsigned long>(bytes[off]) << 24) |
           (static_cast<unsigned long>(bytes[off + 1]) << 16) |
           (static_cast<unsigned long>(bytes[off + 2]) << 8) |
           static_cast<unsigned long>(bytes[off + 3]);
  };
  CHECK(be32(16) == 160);
  CHECK(be32(20) == 120);

  // An unwritable destination fails cleanly without disturbing playback.
  CHECK_FALSE(ffmpeg->saveScreenshot("/nonexistent_dir_soar/shot.png"));
  CHECK_FALSE(ffmpeg->lastError().empty());
  CHECK(backend->position() >= 0ms);

  // A destination that accepts the open but refuses the bytes (/dev/full,
  // present on Linux and macOS) fails through the write-mismatch arm
  // instead of the open arm above.
  if (::access("/dev/full", W_OK) == 0) {
    CHECK_FALSE(ffmpeg->saveScreenshot("/dev/full"));
    CHECK_FALSE(ffmpeg->lastError().empty());
    CHECK(backend->position() >= 0ms);
  }

  // After close() the retained frame is released; a screenshot attempt
  // after stop() + close() must fail the presented-frame guard.
  backend->stop();
  backend->close();
  CHECK_FALSE(ffmpeg->saveScreenshot("after_close.png"));
  CHECK_FALSE(ffmpeg->lastError().empty());

  ::unlink(out.c_str());
  ::unlink("after_close.png");
}

TEST_CASE("saveScreenshot forces PNG encoder open failure") {
  // The PNG encoder is normally always available. To exercise the
  // avcodec_open2 failure arm (saveScreenshot internal), we pass
  // forceFailEncoder=true which configures the context with an
  // invalid pixel format, making avcodec_open2 reject it.
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping screenshot encoder-fail test");
    return;
  }

  auto backend = soar::makeFFmpegBackend();
  auto* ffmpeg = static_cast<soar::FFmpegBackend*>(backend.get());

  REQUIRE(backend->open(soar::MediaSource{media}));
  REQUIRE(backend->play());

  // Retry until a frame is presented.
  bool saved = false;
  for (int i = 0; i < 1000 && !saved; ++i) {
    saved = ffmpeg->saveScreenshot("normal.png");
    if (!saved) std::this_thread::sleep_for(10ms);
  }
  REQUIRE(saved);
  ::unlink("normal.png");

  // With forceFailEncoder, avcodec_open2 fails and the call records
  // the error without writing a file.
  CHECK_FALSE(ffmpeg->saveScreenshot("forced_fail.png", true));
  CHECK_FALSE(ffmpeg->lastError().empty());
  CHECK(::access("forced_fail.png", F_OK) != 0);
  CHECK(backend->position() >= 0ms);

  backend->stop();
  backend->close();
  ::unlink("forced_fail.png");
}

TEST_CASE("audio output devices enumerate and switch without media") {
  CountingSink sink;
  auto backend = soar::makeFFmpegBackend();
  backend->setEventSink(&sink);

  // Selection is endpoint state, not media state: it works before open.
  // "" means system default and is always valid.
  CHECK(backend->currentAudioOutputDevice().empty());
  CHECK(backend->selectAudioOutputDevice(""));

  // Unknown names are a UI-level mistake: lastError() only, no Error
  // event (the saveScreenshot precedent), no selection change.
  CHECK_FALSE(backend->selectAudioOutputDevice("soar-no-such-device"));
  CHECK(backend->lastError().find("selectAudioOutputDevice") != std::string::npos);
  CHECK(sink.errors.load() == 0);
  CHECK(backend->currentAudioOutputDevice().empty());
  CHECK(backend->state() == soar::PlaybackState::Stopped);

  // Whatever the host enumerates (the dummy driver reports one device;
  // a runner without any audio stack may report none), every listed
  // name is selectable and sticks.
  for (const auto& name : backend->audioOutputDevices()) {
    INFO("device: ", name);
    CHECK(backend->selectAudioOutputDevice(name));
    CHECK(backend->currentAudioOutputDevice() == name);
  }
  CHECK(backend->selectAudioOutputDevice(""));
  CHECK(backend->currentAudioOutputDevice().empty());
  CHECK(sink.errors.load() == 0);
}

TEST_CASE("output device switch during playback keeps playing") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping device-switch test");
    return;
  }

  CountingSink sink;
  auto backend = soar::makeFFmpegBackend();
  backend->setEventSink(&sink);

  // Pick a concrete endpoint *before* the first frame: that makes the
  // decode thread open that device by name instead of the default one.
  const auto devices = backend->audioOutputDevices();
  if (!devices.empty()) {
    REQUIRE(backend->selectAudioOutputDevice(devices.front()));
  }

  REQUIRE(backend->open(soar::MediaSource{media}));
  REQUIRE(backend->play());
  REQUIRE(waitForPosition(*backend, 1ms, 30s));
  const auto before = backend->position();

  // Live switch back to the default: the open endpoint is re-opened on
  // the spot, the decode thread re-fills the new queue, the clock never
  // stops.
  REQUIRE(backend->selectAudioOutputDevice(""));
  CHECK(backend->state() == soar::PlaybackState::Playing);
  CHECK(waitForPosition(*backend, before + 200ms, 30s));

  // And back to the concrete device, still playing.
  if (!devices.empty()) {
    REQUIRE(backend->selectAudioOutputDevice(devices.front()));
    CHECK(backend->state() == soar::PlaybackState::Playing);
    const auto after = backend->position();
    CHECK(waitForPosition(*backend, after + 200ms, 30s));
  }
  CHECK(sink.errors.load() == 0);

  backend->stop();
  backend->close();

  // Selection outlives the media: the endpoint stays as it was chosen,
  // and it is what the next open will use.
  if (!devices.empty()) {
    CHECK(backend->currentAudioOutputDevice() == devices.front());
  }
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

TEST_CASE("a backward seek while playing re-emits the rewound position") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping backward-seek test");
    return;
  }

  // The position event is throttled by a granularity window in both
  // directions: while playing, a seek that jumps *back* by more than the
  // window must still emit, otherwise the UI's progress bar would keep
  // showing the old (later) position until the next forward tick.
  CountingSink sink;
  auto backend = soar::makeFFmpegBackend();
  backend->setEventSink(&sink);

  REQUIRE(backend->open(soar::MediaSource{media}));
  REQUIRE(backend->play());
  std::this_thread::sleep_for(300ms);

  REQUIRE(backend->seek(std::chrono::milliseconds(4000)));
  bool forward = false;
  for (int i = 0; i < 500 && !forward; ++i) {
    forward = backend->position() >= std::chrono::milliseconds(4000);
    if (!forward) {
      std::this_thread::sleep_for(10ms);
    }
  }
  REQUIRE(forward);
  const int after_forward = sink.position_changed.load();

  // Rewind to the start: well past the granularity window, so a new
  // PositionChanged has to be emitted for the rewind itself.
  REQUIRE(backend->seek(std::chrono::milliseconds(0)));
  bool rewound = false;
  for (int i = 0; i < 500 && !rewound; ++i) {
    rewound = backend->position() < std::chrono::milliseconds(1000);
    if (!rewound) {
      std::this_thread::sleep_for(10ms);
    }
  }
  CHECK(rewound);
  CHECK(sink.position_changed.load() > after_forward);

  backend->stop();
  backend->close();
  CHECK(sink.errors.load() == 0);
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

TEST_CASE("subtitle packets decode to text frames during playback") {
  std::string media;
  if (!envMedia("SOAR_TEST_SUBS_MEDIA", media)) {
    MESSAGE("SOAR_TEST_SUBS_MEDIA not set; skipping subtitle decode test");
    return;
  }

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{media}));
  REQUIRE(backend->play());

  // The fixture's SRT carries "Hello" at 0-2s and "World" at 2-4s. The SRT
  // decoder emits ASS-format subtitle rects, so this also pins the ASS
  // wrapper stripping (a raw data[0] dump would show "Dialogue:" markup
  // instead of the words).
  std::string first, second;
  auto* ffmpeg = static_cast<soar::FFmpegBackend*>(backend.get());
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline && second.empty()) {
    soar::DecodedSubtitleFrame frame;
    if (ffmpeg->tryGetSubtitleFrame(frame)) {
      if (first.empty()) {
        first = frame.text;
      } else {
        second = frame.text;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  backend->stop();
  backend->close();

  CHECK(first.find("Hello") != std::string::npos);
  CHECK(second.find("World") != std::string::npos);
}

TEST_CASE("ASS cues decode through the Dialogue parser") {
  std::string media;
  if (!envMedia("SOAR_TEST_ASS_MEDIA", media)) {
    MESSAGE("SOAR_TEST_ASS_MEDIA not set; skipping ASS decode test");
    return;
  }

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{media}));
  REQUIRE(backend->play());

  // The ASS fixture's first cue reads "{\i1}Styled{\i0}\NLine": the ass
  // decoder hands over full "Dialogue: ..." event lines (nine commas, unlike
  // the eight-comma synthesized lines the SRT decoder produces), and the
  // text extraction must drop the override blocks and turn the hard break
  // into a space. A markup leak ("{\i1}" or "\N") would fail the checks
  // below even though the raw rects carried the words.
  std::string first, second;
  auto* ffmpeg = static_cast<soar::FFmpegBackend*>(backend.get());
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline && second.empty()) {
    soar::DecodedSubtitleFrame frame;
    if (ffmpeg->tryGetSubtitleFrame(frame)) {
      if (first.empty()) {
        first = frame.text;
      } else {
        second = frame.text;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  backend->stop();
  backend->close();

#ifdef SOAR_WITH_LIBASS
  // Style-faithful build: an embedded ASS track feeds the libass renderer
  // (backend->assRenderer()) and the plain-text queue stays empty on
  // purpose — the same words decode but surface as glyphs, not text. The
  // rendered pixels are asserted by the styled-fixture case below.
  CHECK(backend->assRenderer() != nullptr);
  CHECK(first.empty());
  CHECK(second.empty());
#else
  CHECK(first.find("Styled") != std::string::npos);
  CHECK(first.find("Line") != std::string::npos);
  CHECK(first.find('{') == std::string::npos);
  CHECK(first.find("\\N") == std::string::npos);
  CHECK(second.find("Second cue") != std::string::npos);
#endif
}

TEST_CASE("mov_text cues decode as plain text rects") {
  std::string media;
  if (!envMedia("SOAR_TEST_MOVTEXT_MEDIA", media)) {
    MESSAGE("SOAR_TEST_MOVTEXT_MEDIA not set; skipping mov_text decode test");
    return;
  }

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{media}));
  REQUIRE(backend->play());

  // The mp4 fixture carries the same cues as mov_text. Whichever rect
  // shape the decoder picks for this container (raw SUBTITLE_TEXT, or the
  // ASS wrapper the text decoders synthesize), the payload that reaches
  // the UI must be the bare words. The third cue spans two lines, so it
  // also pins the hard-break handling: "\n" must not survive into the
  // rendered string as a markup escape.
  std::string first, second, third;
  auto* ffmpeg = static_cast<soar::FFmpegBackend*>(backend.get());
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline && third.empty()) {
    soar::DecodedSubtitleFrame frame;
    if (ffmpeg->tryGetSubtitleFrame(frame)) {
      if (first.empty()) {
        first = frame.text;
      } else if (second.empty()) {
        second = frame.text;
      } else {
        third = frame.text;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  backend->stop();
  backend->close();

  CHECK(first.find("Hello") != std::string::npos);
  CHECK(second.find("World") != std::string::npos);
  CHECK(third.find("Two") != std::string::npos);
  CHECK(third.find("Lines") != std::string::npos);
  // A leaked hard break ("\n", "\\N") or Dialogue field would show up here.
  CHECK(third.find('\n') == std::string::npos);
  CHECK(third.find("\\N") == std::string::npos);
  CHECK(third.find("Dialogue") == std::string::npos);
}

TEST_CASE("a cue with no visible text is never queued") {
  std::string media;
  if (!envMedia("SOAR_TEST_SUBS_EMPTY", media)) {
    MESSAGE("SOAR_TEST_SUBS_EMPTY not set; skipping empty-cue test");
    return;
  }

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{media}));
  REQUIRE(backend->play());

  // The first cue of the fixture is "{\i1}" — pure ASS markup. The text
  // extraction strips the override block and is left with nothing, so the
  // frame must be dropped instead of queued as an empty payload the UI
  // would later try to render. The second cue ("Visible") is the control:
  // reaching it proves the stream decoded and the extraction path ran, so
  // the absence of an empty frame is a real result and not a dead stream.
  std::vector<std::string> pulled;
  auto* ffmpeg = static_cast<soar::FFmpegBackend*>(backend.get());
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline && pulled.empty()) {
    soar::DecodedSubtitleFrame frame;
    if (ffmpeg->tryGetSubtitleFrame(frame)) {
      pulled.push_back(frame.text);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  backend->stop();
  backend->close();

#ifdef SOAR_WITH_LIBASS
  // Style-faithful build: the whole ASS stream routes to the libass
  // renderer — the empty markup cue included (libass tolerates it; the
  // event just paints nothing). The plain-text queue stays empty either
  // way, which is the contract this test guards.
  CHECK(backend->assRenderer() != nullptr);
  CHECK(pulled.empty());
#else
  REQUIRE(pulled.size() == 1);
  CHECK(pulled[0].find("Visible") != std::string::npos);
  CHECK(pulled[0].find('{') == std::string::npos);
#endif
}

TEST_CASE("embedded ASS renders style-faithfully through libass") {
  std::string media;
  if (!envMedia("SOAR_TEST_STYLED_ASS_MEDIA", media)) {
    MESSAGE("SOAR_TEST_STYLED_ASS_MEDIA not set; skipping styled ASS test");
    return;
  }

  auto backend = soar::makeFFmpegBackend();
  auto* ffmpeg = static_cast<soar::FFmpegBackend*>(backend.get());
  auto* ass = backend->assRenderer();
#ifndef SOAR_WITH_LIBASS
  // No libass in this build: the documented null renderer, and the
  // plain-text path keeps running (covered by the cases above).
  CHECK(ass == nullptr);
  MESSAGE("libass not compiled in; skipping styled ASS rendering");
  return;
#else
  REQUIRE(ass != nullptr);
  REQUIRE(backend->open(soar::MediaSource{media}));
  REQUIRE(backend->play());

  // The first cue is visible from t=0 (it spans 0-2 s): poll until the
  // decode thread has fed libass and the composite comes back non-empty.
  soar::AssFrame f;
  bool rendered = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline && !rendered) {
    rendered = ass->renderAt(ffmpeg->position().count(), &f);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  REQUIRE(rendered);
  CHECK(f.width == 160);
  CHECK(f.height == 120);

  // The script's only style is an opaque pure-red fill (&H000000FF in the
  // &HAABBGGRR file convention) with outline and shadow off, and the
  // canvas is straight-alpha RGBA — so every composited pixel must be
  // exactly red with per-pixel coverage as its alpha. A channel swap or a
  // premultiplied alpha leaks through here as a non-red channel.
  std::size_t visible = 0;
  bool all_red = true;
  for (std::size_t i = 0; i + 3 < f.rgba.size(); i += 4) {
    if (f.rgba[i + 3] == 0) continue;
    ++visible;
    if (f.rgba[i] != 255 || f.rgba[i + 1] != 0 || f.rgba[i + 2] != 0) {
      all_red = false;
    }
  }
  CHECK(visible > 50);
  CHECK(all_red);

  // Double-render suppression: a styled track never queues plain text.
  soar::DecodedSubtitleFrame sf;
  CHECK_FALSE(ffmpeg->tryGetSubtitleFrame(sf));

  // Seek past the last cue (it ends at 4 s): the flush on seek drops the
  // fed events, and the canvas must report the loss exactly once.
  REQUIRE(backend->seek(std::chrono::milliseconds(4500)));
  bool vanished = false;
  const auto vanish_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < vanish_deadline && !vanished) {
    vanished = !ass->renderAt(ffmpeg->position().count(), &f) && f.changed;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK(vanished);
  CHECK(f.rgba.empty());

  backend->stop();
  backend->close();
#endif
}

// Pure green in the &HAABBGGRR file convention — a color the fixture's
// embedded red script never produces, so pixel assertions below can only
// come from the external document.
bool isAllGreen(const soar::AssFrame& f) {
  bool any = false;
  for (std::size_t i = 0; i + 3 < f.rgba.size(); i += 4) {
    if (f.rgba[i + 3] == 0) continue;
    any = true;
    if (f.rgba[i + 0] != 0 || f.rgba[i + 1] != 255 || f.rgba[i + 2] != 0) {
      return false;
    }
  }
  return any;
}

bool isAllRed(const soar::AssFrame& f) {
  bool any = false;
  for (std::size_t i = 0; i + 3 < f.rgba.size(); i += 4) {
    if (f.rgba[i + 3] == 0) continue;
    any = true;
    if (f.rgba[i + 0] != 255 || f.rgba[i + 1] != 0 || f.rgba[i + 2] != 0) {
      return false;
    }
  }
  return any;
}

// The synthesized document's own style is white-on-outline, so the pixel
// oracle for it is "the canvas has visible glyphs at all" — the color is
// the product's default, not the fixture's.
bool hasVisible(const soar::AssFrame& f) {
  for (std::size_t i = 3; i < f.rgba.size(); i += 4) {
    if (f.rgba[i] != 0) return true;
  }
  return false;
}

// The external document every case in this group loads: 160x120 PlayRes,
// one "Green" style over the same fixture face the ass unit tests use,
// and two cues covering the whole 6 s fixture so the polls have slack on
// a slow machine.
const char* kExternalDocCues =
    "Dialogue: 0,0:00:00.00,0:00:03.00,Green,,0,0,0,,Doc One\n"
    "Dialogue: 0,0:00:03.00,0:00:06.00,Green,,0,0,0,,Doc Two\n";

std::string makeExternalDoc() {
  return
      "[Script Info]\n"
      "; soar external-document test script\n"
      "ScriptType: v4.00+\n"
      "PlayResX: 160\n"
      "PlayResY: 120\n"
      "WrapStyle: 0\n"
      "ScaledBorderAndShadow: yes\n"
      "\n"
      "[V4+ Styles]\n"
      "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
      "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, "
      "ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
      "Alignment, MarginL, MarginR, MarginV, Encoding\n"
      "Style: Green,Noto Mono,24,&H0000FF00,&H00FFFFFF,&H00000000,&H00000000,"
      "0,0,0,0,100,100,0,0,1,0,0,2,10,10,10,1\n"
      "\n"
      "[Events]\n"
      "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, "
      "Effect, Text\n" + std::string(kExternalDocCues);
}

TEST_CASE("an external .ass document loads as a track and renders per build") {
  // A media without any embedded subtitle stream (same 160x120 video the
  // styled fixture carries): every frame or pixel below can only have
  // come from the external document, in both builds.
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping external document test");
    return;
  }
  ScratchDir dir;
  const std::string local = dir.copyIn(media, "movie.mkv");
  REQUIRE_FALSE(local.empty());
  dir.write("movie.ass", makeExternalDoc());

  auto backend = soar::makeFFmpegBackend();
  auto* ffmpeg = static_cast<soar::FFmpegBackend*>(backend.get());
  auto* ass = backend->assRenderer();
  REQUIRE(backend->open(soar::MediaSource{local}));

  // A document sidecar is a first-class track shaped like any other: the
  // codec names the format, the title is the file name. The track list is
  // copied out first — a pointer into the mediaInfo() temporary would
  // dangle the moment the expression ends (ASAN/TSAN catch exactly that).
  soar::TrackId id = -1;
  REQUIRE(backend->loadExternalSubtitle(dir.file("movie.ass"), id));
  const auto tracks = backend->mediaInfo().tracks;
  const soar::TrackInfo* ext = nullptr;
  for (const auto& t : tracks) {
    if (t.id == id) ext = &t;
  }
  REQUIRE(ext != nullptr);
  CHECK(ext->type == soar::TrackType::Subtitle);
  CHECK(ext->codec == "ass");
  CHECK(ext->title == "movie.ass");

  // Opt-in like every track: loading does not mean showing.
  CHECK(backend->mediaInfo().selected_subtitle == -1);
  CHECK(backend->selectTrack(soar::TrackType::Subtitle, id));
  CHECK(backend->mediaInfo().selected_subtitle == id);

  REQUIRE(backend->play());
#ifndef SOAR_WITH_LIBASS
  // No libass: the renderer is the documented null and the Dialogue
  // extraction became the track — the plain-text pump serves the cues.
  CHECK(ass == nullptr);
  const auto frames = pullSubtitleFrames(ffmpeg, 2, std::chrono::seconds(30));
  REQUIRE(frames.size() >= 2);
  CHECK(frames[0].text.find("Doc One") != std::string::npos);
  CHECK(frames[1].text.find("Doc Two") != std::string::npos);
#else
  // Document mode: the pump stays idle — the embedded stream's events are
  // deselected and the extraction is only a load gate, so a plain-text
  // frame here would mean the document gets drawn twice.
  REQUIRE(ass != nullptr);
  ass->setDefaultFont(SOAR_TEST_FONT_FILE);
  CHECK(pullSubtitleFrames(ffmpeg, 1, std::chrono::milliseconds(400)).empty());

  // The canvas follows the playhead in the document's own style.
  soar::AssFrame f;
  bool rendered = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline && !rendered) {
    rendered = ass->renderAt(ffmpeg->position().count(), &f);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  REQUIRE(rendered);
  CHECK(f.width == 160);
  CHECK(f.height == 120);
  CHECK(isAllGreen(f));
  soar::DecodedSubtitleFrame sf;
  CHECK_FALSE(ffmpeg->tryGetSubtitleFrame(sf));
#endif
  backend->stop();
  backend->close();
}

TEST_CASE("an external .ass file without dialogue lines is refused") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping empty document test");
    return;
  }
  ScratchDir dir;
  const std::string local = dir.copyIn(media, "movie.mkv");
  REQUIRE_FALSE(local.empty());
  // A well-formed script header with no [Events] record extracts to no
  // cue at all — loading it would promise a track that can never show
  // anything, so the load fails instead.
  dir.write("empty.ass",
            "[Script Info]\n"
            "ScriptType: v4.00+\n"
            "PlayResX: 160\n"
            "PlayResY: 120\n");

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{local}));
  soar::TrackId id = -1;
  CHECK_FALSE(backend->loadExternalSubtitle(dir.file("empty.ass"), id));
  CHECK(id == -1);
  CHECK(backend->mediaInfo().tracks.size() == 3);  // video + dual audio only
  backend->close();
}

TEST_CASE("an SRT sidecar whose only cue carries no payload is refused") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping payload-less sidecar test");
    return;
  }
  ScratchDir dir;
  const std::string local = dir.copyIn(media, "movie.mkv");
  REQUIRE_FALSE(local.empty());
  // detectSubtitleFormat only needs a parseable head timestamp to
  // declare SubRip — it never looks at payload lines — while the parser
  // drops a timestamp pair carrying no text. The two rules together make
  // a detectable file that parses to zero cues, and the load must refuse
  // it instead of registering a track that can never show anything.
  dir.write("nopayload.srt",
            "1\n"
            "00:00:01,000 --> 00:00:02,000\n");

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{local}));
  const std::size_t before = backend->mediaInfo().tracks.size();
  soar::TrackId id = -1;
  CHECK_FALSE(backend->loadExternalSubtitle(dir.file("nopayload.srt"), id));
  CHECK(id == -1);
  CHECK(backend->lastError().find("no cues") != std::string::npos);
  CHECK(backend->mediaInfo().tracks.size() == before);
  backend->close();
}

#ifdef SOAR_WITH_LIBASS

TEST_CASE("selecting the embedded ASS stream back restores the libass feed") {
  std::string media;
  if (!envMedia("SOAR_TEST_STYLED_ASS_MEDIA", media)) {
    MESSAGE("SOAR_TEST_STYLED_ASS_MEDIA not set; skipping feed restore test");
    return;
  }
  ScratchDir dir;
  const std::string local = dir.copyIn(media, "movie.mkv");
  REQUIRE_FALSE(local.empty());
  dir.write("movie.ass", makeExternalDoc());

  auto backend = soar::makeFFmpegBackend();
  auto* ffmpeg = static_cast<soar::FFmpegBackend*>(backend.get());
  auto* ass = backend->assRenderer();
  REQUIRE(ass != nullptr);
  ass->setDefaultFont(SOAR_TEST_FONT_FILE);
  REQUIRE(backend->open(soar::MediaSource{local}));
  REQUIRE(backend->play());

  // Baseline: the embedded script renders its red (batch 1a behavior, the
  // document has not been loaded yet).
  soar::AssFrame f;
  bool red = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline && !red) {
    red = ass->renderAt(ffmpeg->position().count(), &f) && isAllRed(f);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  REQUIRE(red);

  // Detour through the document: the canvas turns green.
  soar::TrackId doc_id = -1;
  REQUIRE(backend->loadExternalSubtitle(dir.file("movie.ass"), doc_id));
  CHECK(backend->selectTrack(soar::TrackType::Subtitle, doc_id));
  bool green = false;
  const auto green_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < green_deadline && !green) {
    green = ass->renderAt(ffmpeg->position().count(), &f) && isAllGreen(f);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  REQUIRE(green);

  // Back to the embedded stream: its id sits below every external id.
  soar::TrackId embedded_id = -1;
  for (const auto& t : backend->mediaInfo().tracks) {
    if (t.type == soar::TrackType::Subtitle &&
        (embedded_id < 0 || t.id < embedded_id)) {
      embedded_id = t.id;
    }
  }
  REQUIRE(embedded_id >= 0);
  CHECK(backend->selectTrack(soar::TrackType::Subtitle, embedded_id));

  // Events resume from the decoder's read position, so the seek to the
  // top is what makes cue one deterministic: it flushes the feed and the
  // decoder rescans from the first packet. Retry the seek if the poll
  // overshoots the cue's two-second window on a loaded machine.
  red = false;
  for (int attempt = 0; attempt < 5 && !red; ++attempt) {
    REQUIRE(backend->seek(std::chrono::milliseconds(0)));
    const auto red_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < red_deadline && !red) {
      red = ass->renderAt(ffmpeg->position().count(), &f) && isAllRed(f);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  CHECK(red);

  backend->stop();
  backend->close();
}

TEST_CASE("a text sidecar takes the canvas over an embedded ASS stream") {
  // The batch-1b known boundary — an embedded ASS stream and a text
  // sidecar drawing at once — closes for libass builds in batch 1c:
  // selecting the SRT sidecar redirects the renderer to a synthesized
  // document and the document-mode gate drops the embedded events, so
  // the canvas stops being the fixture's red and shows the synthesized
  // default style instead. (The sidecar's queue frames keep flowing —
  // the no-double-draw gate is the UI's subtitleDocumentActive() skip. A
  // no-libass build has no canvas; since the selection-semantics batch
  // the embedded decode gate closes at the source there too — the
  // sidecar-selection case in this suite pins it through the queue.)
  std::string media;
  if (!envMedia("SOAR_TEST_STYLED_ASS_MEDIA", media)) {
    MESSAGE("SOAR_TEST_STYLED_ASS_MEDIA not set; skipping overlay test");
    return;
  }
  ScratchDir dir;
  const std::string local = dir.copyIn(media, "movie.mkv");
  REQUIRE_FALSE(local.empty());
  // One cue spanning the whole fixture: whichever position the red
  // baseline ends at, the synthesized line is on screen there.
  dir.write("movie.srt",
            "1\n00:00:00,000 --> 00:00:06,000\nplain text wins\n\n");

  auto backend = soar::makeFFmpegBackend();
  auto* ffmpeg = static_cast<soar::FFmpegBackend*>(backend.get());
  auto* ass = backend->assRenderer();
  REQUIRE(ass != nullptr);
  ass->setDefaultFont(SOAR_TEST_FONT_FILE);
  REQUIRE(backend->open(soar::MediaSource{local}));
  REQUIRE(backend->play());

  soar::AssFrame f;
  bool red = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline && !red) {
    red = ass->renderAt(ffmpeg->position().count(), &f) && isAllRed(f);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  REQUIRE(red);

  soar::TrackId id = -1;
  REQUIRE(backend->loadExternalSubtitle(dir.file("movie.srt"), id));
  REQUIRE(backend->selectTrack(soar::TrackType::Subtitle, id));
  bool plain = false;
  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline && !plain) {
    plain = ass->renderAt(ffmpeg->position().count(), &f) && hasVisible(f) &&
            !isAllRed(f);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  REQUIRE(plain);
  backend->stop();
  backend->close();
}

TEST_CASE("disabling subtitles releases a held ASS document") {
  std::string media;
  if (!envMedia("SOAR_TEST_STYLED_ASS_MEDIA", media)) {
    MESSAGE("SOAR_TEST_STYLED_ASS_MEDIA not set; skipping document disable test");
    return;
  }
  ScratchDir dir;
  const std::string local = dir.copyIn(media, "movie.mkv");
  REQUIRE_FALSE(local.empty());
  dir.write("movie.ass", makeExternalDoc());

  auto backend = soar::makeFFmpegBackend();
  auto* ffmpeg = static_cast<soar::FFmpegBackend*>(backend.get());
  auto* ass = backend->assRenderer();
  REQUIRE(ass != nullptr);
  ass->setDefaultFont(SOAR_TEST_FONT_FILE);
  REQUIRE(backend->open(soar::MediaSource{local}));

  soar::TrackId id = -1;
  REQUIRE(backend->loadExternalSubtitle(dir.file("movie.ass"), id));
  CHECK(backend->selectTrack(soar::TrackType::Subtitle, id));
  REQUIRE(backend->play());

  soar::AssFrame f;
  bool green = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline && !green) {
    green = ass->renderAt(ffmpeg->position().count(), &f) && isAllGreen(f);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  REQUIRE(green);

  // "Off" must not leave the document on the canvas — the empty-track
  // swap reports exactly one clearing render.
  CHECK(backend->disableSubtitles());
  bool vanished = false;
  const auto vanish_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < vanish_deadline && !vanished) {
    vanished = !ass->renderAt(ffmpeg->position().count(), &f) && f.changed;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK(vanished);
  CHECK(f.rgba.empty());

  backend->stop();
  backend->close();
}

#endif  // SOAR_WITH_LIBASS

TEST_CASE("a burst of adjacent cues overflows the subtitle FIFO in order") {
  std::string media;
  if (!envMedia("SOAR_TEST_BURST_MEDIA", media)) {
    MESSAGE("SOAR_TEST_BURST_MEDIA not set; skipping subtitle burst test");
    return;
  }

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{media}));
  REQUIRE(backend->play());

  // Six cues inside the first half second of an audio-only file. The
  // contract under test is the depth-4 cap itself: with nothing
  // draining the FIFO while the decode thread queues the burst, the
  // cap drops the two oldest cues and the survivors surface in cue
  // order. Racing the producer instead (polling while it queues) makes
  // the drop load-dependent — a lightly loaded run drains each frame
  // as it lands and all six survive (observed in a local coverage
  // run), a loaded one sees the drop — so wait for the decode thread
  // to reach EOF (Ended) before pulling anything. queueSubtitleFrame
  // drops oldest without ever blocking, so a full queue cannot stall
  // the producer during the wait.
  const auto ended_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (backend->state() != soar::PlaybackState::Ended &&
         std::chrono::steady_clock::now() < ended_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  REQUIRE(backend->state() == soar::PlaybackState::Ended);

  std::vector<std::string> pulled;
  auto* ffmpeg = static_cast<soar::FFmpegBackend*>(backend.get());
  soar::DecodedSubtitleFrame frame;
  while (ffmpeg->tryGetSubtitleFrame(frame)) {
    pulled.push_back(frame.text);
  }

  backend->stop();
  backend->close();

  CHECK(pulled.size() == 4);
  CHECK(pulled[0].find("Cue 3") != std::string::npos);
  CHECK(pulled[1].find("Cue 4") != std::string::npos);
  CHECK(pulled[2].find("Cue 5") != std::string::npos);
  CHECK(pulled[3].find("Cue 6") != std::string::npos);
}

TEST_CASE("a sidecar file loads as a subtitle track and plays") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping sidecar playback test");
    return;
  }
  ScratchDir dir;
  const std::string local = dir.copyIn(media, "movie.mkv");
  REQUIRE_FALSE(local.empty());
  // Two cues in the first two seconds, so the playhead reaches them well
  // before the 6 s fixture ends. The payload is deliberately unlike
  // anything in the fixture: this one has no subtitle stream at all, so
  // every frame pulled below can only have come from the sidecar.
  dir.write("movie.srt",
            "1\n00:00:00,000 --> 00:00:01,000\nSidecar One\n\n"
            "2\n00:00:01,000 --> 00:00:02,000\nSidecar Two\n\n");

  CountingSink sink;
  auto backend = soar::makeFFmpegBackend();
  backend->setEventSink(&sink);
  REQUIRE(backend->open(soar::MediaSource{local}));

  // A sidecar is opt-in, exactly like an embedded track: loading it does
  // not start showing it.
  CHECK(backend->mediaInfo().selected_subtitle == -1);

  soar::TrackId id = -1;
  REQUIRE(backend->loadExternalSubtitle(dir.file("movie.srt"), id));
  CHECK(id >= 0);
  CHECK(sink.media_info_changed.load() >= 1);

  // The track is a first-class entry, shaped like an embedded one: the
  // codec names the format so the menu reads the same for both, the title
  // is the file name, and the language stays empty because a sidecar's
  // language tag is a provider concern carried by the name.
  const auto info = backend->mediaInfo();
  const soar::TrackInfo* ext = nullptr;
  for (const auto& t : info.tracks) {
    if (t.id == id) ext = &t;
  }
  REQUIRE(ext != nullptr);
  CHECK(ext->type == soar::TrackType::Subtitle);
  CHECK(ext->codec == "subrip");
  CHECK(ext->title == "movie.srt");
  CHECK(ext->language.empty());

  REQUIRE(backend->selectTrack(soar::TrackType::Subtitle, id));
  CHECK(backend->mediaInfo().selected_subtitle == id);

  REQUIRE(backend->play());
  const auto frames = pullSubtitleFrames(
    static_cast<soar::FFmpegBackend*>(backend.get()), 2, std::chrono::seconds(30));
  backend->stop();
  backend->close();

  // The cues arrive in file order...
  REQUIRE(frames.size() >= 2);
  CHECK(frames[0].text.find("Sidecar One") != std::string::npos);
  CHECK(frames[1].text.find("Sidecar Two") != std::string::npos);
  // ...and timed from the file, not from the decode clock: the UI shows a
  // frame for `duration` starting at `pts`, so both have to be the file's.
  // (In a libass build these queue frames are the same lines the canvas
  // draws — batch 1c; the plain-text window stays quiet there, and this
  // pull is what still exercises the pump both builds share.)
  CHECK(frames[0].pts == 0ms);
  CHECK(frames[0].duration == 1000ms);
  CHECK(frames[1].pts == 1000ms);
  CHECK(frames[1].duration == 1000ms);
}

TEST_CASE("extraction corners: hard breaks, trailing junk, bracket wrapper") {
  // The embedded-subtitle decode feeds ASS-format rects into the
  // plain-text extraction. One cue per conversion corner: the lowercase
  // hard break, a trailing backslash, trailing whitespace, and the
  // bracket wrapper (twice: the payload-level strip eats one ']'). The
  // rest: the uppercase hard break, a backslash before an ordinary
  // letter (kept as-is), a Dialogue:-shaped payload (the 9-comma
  // full-event form), a comma-ish line, brackets that pop away to
  // nothing, mid-text brace groups (one closed, one left open), and a
  // formatting-only cue the demuxer itself drops.
  // libavcodec passes SRT text through verbatim, so the fixture's
  // characters reach the extractor exactly as written — that lossiness
  // is the documented plain-text contract, not a regression. What the
  // demuxer drops or the extractor empties never surfaces as a frame,
  // so the tail assertions are contentual rather than positional.
  std::string media;
  if (!envMedia("SOAR_TEST_JUNK_SRT_MEDIA", media)) {
    MESSAGE("SOAR_TEST_JUNK_SRT_MEDIA not set; skipping the extraction-corner test");
    return;
  }

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{media}));
  // No selection: the stream is the media's default subtitle stream, and
  // the open state keeps decoding it into the plain-text queue. (The
  // selection would redirect the frames onto the canvas in a libass
  // build — that semantics has its own cases; this one watches the
  // extractor.)
  REQUIRE(backend->play());
  // Thirteen cues decode; two never surface (the formatting-only cue the
  // demuxer drops, and the all-brackets cue extraction empties), so
  // eleven frames arrive.
  const auto frames = pullSubtitleFrames(
    static_cast<soar::FFmpegBackend*>(backend.get()), 11, std::chrono::seconds(30));
  backend->stop();
  backend->close();

  // The first seven cues survive intact, in order; later cues may be
  // dropped by the demuxer (formatting-only) or emptied by extraction.
  REQUIRE(frames.size() >= 7);
  CHECK(frames[0].text == "Hard break");            // \n -> space
  CHECK(frames[1].text == "Trailing backslash");    // trailing '\' -> space, trimmed
  CHECK(frames[2].text == "Padded");                // trailing space trimmed
  CHECK(frames[3].text == "Double");                // ']' wrapper stripped twice over
  CHECK(frames[4].text == "Upper Case");            // \N -> space, either case
  CHECK(frames[5].text.find("Path") == 0);          // '\C' etc. keep the backslash
  CHECK(frames[6].text.find("From SRT") != std::string::npos);  // full-event form
  CHECK(contains(frames, "just,four,words"));
  CHECK(contains(frames, "Ends in brackets"));      // trailing ']]' pops clean
  // '{b}' strips out whole; doctest wants the || outside the CHECK.
  const bool brace_group_gone = contains(frames, "See  here") ||
                                contains(frames, "See here");
  CHECK(brace_group_gone);
  CHECK(contains(frames, "Unclosed"));
}

TEST_CASE("an external subtitle id sits past the container's stream range") {
  std::string media;
  if (!envMedia("SOAR_TEST_SUBS_MEDIA", media)) {
    MESSAGE("SOAR_TEST_SUBS_MEDIA not set; skipping external id test");
    return;
  }
  ScratchDir dir;
  const std::string local = dir.copyIn(media, "clip.mkv");
  REQUIRE_FALSE(local.empty());
  dir.write("clip.en.srt", "1\n00:00:00,000 --> 00:00:02,000\nEnglish\n\n");
  dir.write("clip.zh.vtt", "WEBVTT\n\n00:00:00.000 --> 00:00:02.000\nChinese\n\n");

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{local}));

  // This fixture does carry a subtitle stream of its own (plus an attached
  // text file, which track enumeration must ignore), which is exactly the
  // case where an id scheme that reused stream indices would collide.
  const auto info = backend->mediaInfo();
  soar::TrackId max_embedded = -1;
  int subtitle_streams = 0;
  std::vector<soar::TrackId> embedded_ids;
  for (const auto& t : info.tracks) {
    max_embedded = std::max(max_embedded, t.id);
    embedded_ids.push_back(t.id);
    if (t.type == soar::TrackType::Subtitle) ++subtitle_streams;
  }
  REQUIRE(subtitle_streams == 1);

  soar::TrackId en = -1, zh = -1;
  REQUIRE(backend->loadExternalSubtitle(dir.file("clip.en.srt"), en));
  REQUIRE(backend->loadExternalSubtitle(dir.file("clip.zh.vtt"), zh));

  // Two sidecars take two consecutive slots, both clear of the container's
  // own range.
  CHECK(zh == en + 1);
  CHECK(en > max_embedded);

  // Neither lands on an id the container was already using.
  for (const soar::TrackId id : embedded_ids) {
    CHECK(id != en);
    CHECK(id != zh);
  }

  // The format is read from the content, not the extension, so a WebVTT
  // sidecar is labelled as one even though the menu cannot tell formats
  // apart at a glance.
  // Take the snapshot into a named object: TrackInfo pointers into the
  // temporary returned by mediaInfo() would dangle the moment the
  // full-expression ends (tsan/asan heap-use-after-free otherwise).
  const auto loaded = backend->mediaInfo();
  const soar::TrackInfo* en_track = nullptr;
  const soar::TrackInfo* zh_track = nullptr;
  for (const auto& t : loaded.tracks) {
    if (t.id == en) en_track = &t;
    if (t.id == zh) zh_track = &t;
  }
  REQUIRE(en_track != nullptr);
  REQUIRE(zh_track != nullptr);
  CHECK(en_track->codec == "subrip");
  CHECK(zh_track->codec == "webvtt");
  CHECK(en_track->title == "clip.en.srt");
  CHECK(zh_track->title == "clip.zh.vtt");

  // Both ids are accepted by selectTrack, which is the functional proof
  // that they live outside the embedded range the old check covered.
  CHECK(backend->selectTrack(soar::TrackType::Subtitle, en));
  CHECK(backend->selectTrack(soar::TrackType::Subtitle, zh));
  CHECK(backend->mediaInfo().selected_subtitle == zh);

  // An id past the loaded sidecars is still unknown, and so is the
  // container's own attachment stream, which is in range but is not a
  // subtitle. Neither may be selectable just because external ids widened
  // the range.
  CHECK_FALSE(backend->selectTrack(soar::TrackType::Subtitle, zh + 1));
  CHECK_FALSE(backend->selectTrack(soar::TrackType::Subtitle, zh + 100));
  CHECK(backend->mediaInfo().selected_subtitle == zh);

  backend->close();
}

TEST_CASE("discovery and loading agree on what a sidecar is") {
  std::string media;
  if (!envMedia("SOAR_TEST_SUBS_MEDIA", media)) {
    MESSAGE("SOAR_TEST_SUBS_MEDIA not set; skipping discovery test");
    return;
  }
  ScratchDir dir;
  const std::string local = dir.copyIn(media, "clip.mkv");
  REQUIRE_FALSE(local.empty());
  dir.write("clip.en.srt", "1\n00:00:00,000 --> 00:00:02,000\nEnglish\n\n");
  dir.write("clip.zh.vtt", "WEBVTT\n\n00:00:00.000 --> 00:00:02.000\nChinese\n\n");
  // A bitmap subtitle needs a decoder, not a parser, so the provider must
  // never offer it — and the backend must never be handed one.
  dir.write("clip.pgs.sup", "\x50\x47\x53");

  // The provider is what the menu lists, so the list and what the backend
  // accepts have to be the same set of files.
  const soar::SidecarSubtitleProvider provider;
  const auto candidates = provider.findCandidates(soar::MediaSource{local, {}});
  REQUIRE(candidates.size() == 2);

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{local}));

  for (const auto& c : candidates) {
    soar::TrackId id = -1;
    CHECK(backend->loadExternalSubtitle(c.path, id));
    CHECK(id >= 0);
    // What the provider labelled is what the track is called, which is how
    // the menu tells a loaded sidecar from a still-offered one.
    const auto info = backend->mediaInfo();
    const soar::TrackInfo* match = nullptr;
    for (const auto& t : info.tracks) {
      if (t.id == id) match = &t;
    }
    REQUIRE(match != nullptr);
    CHECK(match->title == c.title);
  }

  backend->close();
}

TEST_CASE("a sidecar that cannot be used leaves the media alone") {
  CountingSink sink;
  auto backend = soar::makeFFmpegBackend();
  backend->setEventSink(&sink);

  // No media: nothing to attach a subtitle to.
  soar::TrackId id = 999;
  CHECK_FALSE(backend->loadExternalSubtitle("/tmp/soar-absent.srt", id));
  CHECK(id == -1);
  CHECK(backend->lastError().find("no media") != std::string::npos);

  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping sidecar failure test");
    return;
  }
  ScratchDir dir;
  const std::string local = dir.copyIn(media, "clip.mkv");
  REQUIRE_FALSE(local.empty());
  dir.write("prose.txt", "this file is not a subtitle at all\n");
  dir.write("one.srt", "1\n00:00:00,000 --> 00:00:01,000\nOnly one\n\n");
  REQUIRE(backend->open(soar::MediaSource{local}));

  // A path that does not exist.
  id = 999;
  CHECK_FALSE(backend->loadExternalSubtitle(dir.file("absent.srt"), id));
  CHECK(id == -1);
  CHECK(backend->lastError().find("cannot read") != std::string::npos);

  // A directory is a path that exists and still is not a subtitle file.
  id = 999;
  CHECK_FALSE(backend->loadExternalSubtitle(dir.path, id));
  CHECK(id == -1);

  // A real file with nothing parseable in it: worth reporting (the user
  // picked it) but not worth a track.
  id = 999;
  CHECK_FALSE(backend->loadExternalSubtitle(dir.file("prose.txt"), id));
  CHECK(id == -1);

  // A file whose cues are all unparseable lands in the same place.
  dir.write("broken.srt", "1\nnot a timestamp at all\nHello\n\n2\n??? --> ???\nWorld\n\n");
  id = 999;
  CHECK_FALSE(backend->loadExternalSubtitle(dir.file("broken.srt"), id));
  CHECK(id == -1);
  CHECK(backend->lastError().find("no cues") != std::string::npos);

  // None of that invented a track...
  CHECK(backend->mediaInfo().selected_subtitle == -1);
  for (const auto& t : backend->mediaInfo().tracks) {
    CHECK(t.type != soar::TrackType::Subtitle);
  }
  // ...and none of it raised an Error event: a missing or broken sidecar is
  // a normal outcome, so the media simply plays without that subtitle.
  CHECK(sink.errors.load() == 0);

  // Playback is untouched by any of it.
  REQUIRE(backend->play());
  CHECK(waitForPosition(*backend, 500ms, std::chrono::seconds(30)));
  backend->stop();
  backend->close();
}

TEST_CASE("reloading a sidecar replaces it in place") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping sidecar reload test");
    return;
  }
  ScratchDir dir;
  const std::string local = dir.copyIn(media, "clip.mkv");
  REQUIRE_FALSE(local.empty());
  const std::string path = dir.file("clip.srt");
  dir.write("clip.srt", "1\n00:00:00,000 --> 00:00:01,000\nVersion One\n\n");

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{local}));

  soar::TrackId first = -1, again = -1;
  REQUIRE(backend->loadExternalSubtitle(path, first));
  // Same path, different content: the menu lets a user re-pick a sidecar
  // (after fixing a bad download, say), and that must not pile up a second
  // track or move the first one.
  dir.write("clip.srt", "1\n00:00:00,000 --> 00:00:01,000\nVersion Two\n\n");
  REQUIRE(backend->loadExternalSubtitle(path, again));
  CHECK(again == first);

  int titled = 0;
  for (const auto& t : backend->mediaInfo().tracks) {
    if (t.title == "clip.srt") ++titled;
  }
  CHECK(titled == 1);

  // The replacement is what plays: the old cues are gone, not shadowed.
  REQUIRE(backend->selectTrack(soar::TrackType::Subtitle, again));
  REQUIRE(backend->play());
  const auto frames = pullSubtitleFrames(
    static_cast<soar::FFmpegBackend*>(backend.get()), 1, std::chrono::seconds(30));
  backend->stop();
  backend->close();

  REQUIRE(frames.size() >= 1);
  CHECK(frames[0].text.find("Version Two") != std::string::npos);
  CHECK(frames[0].text.find("Version One") == std::string::npos);
}

TEST_CASE("closing the media drops its sidecars") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping sidecar lifecycle test");
    return;
  }
  ScratchDir dir;
  const std::string local = dir.copyIn(media, "clip.mkv");
  REQUIRE_FALSE(local.empty());
  dir.write("clip.srt", "1\n00:00:00,000 --> 00:00:01,000\nHello\n\n");
  const std::string path = dir.file("clip.srt");

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{local}));
  soar::TrackId before = -1;
  REQUIRE(backend->loadExternalSubtitle(path, before));

  backend->close();

  // A sidecar belongs to the media that was open: the next open gets the
  // ones next to *its* file. Reopening the same path therefore hands out
  // the same id again instead of continuing a count that outlived the
  // media, and the track list starts clean.
  REQUIRE(backend->open(soar::MediaSource{local}));
  CHECK(backend->mediaInfo().selected_subtitle == -1);
  int titled = 0;
  for (const auto& t : backend->mediaInfo().tracks) {
    if (t.title == "clip.srt") ++titled;
  }
  CHECK(titled == 0);

  soar::TrackId after = -1;
  REQUIRE(backend->loadExternalSubtitle(path, after));
  CHECK(after == before);

  backend->close();
}

TEST_CASE("a backward seek replays the sidecar from the top") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping sidecar seek test");
    return;
  }
  ScratchDir dir;
  const std::string local = dir.copyIn(media, "movie.mkv");
  REQUIRE_FALSE(local.empty());
  dir.write("movie.srt",
            "1\n00:00:00,000 --> 00:00:01,000\nSidecar One\n\n"
            "2\n00:00:02,000 --> 00:00:03,000\nSidecar Two\n\n"
            "3\n00:00:04,000 --> 00:00:05,000\nSidecar Three\n\n");

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{local}));
  soar::TrackId id = -1;
  REQUIRE(backend->loadExternalSubtitle(dir.file("movie.srt"), id));
  REQUIRE(backend->selectTrack(soar::TrackType::Subtitle, id));
  REQUIRE(backend->play());
  auto* ffmpeg = static_cast<soar::FFmpegBackend*>(backend.get());

  // Play past the first two cues so the pump cursor has moved on.
  auto frames = pullSubtitleFrames(ffmpeg, 2, std::chrono::seconds(30));
  REQUIRE(frames.size() >= 2);
  CHECK(frames[0].text.find("Sidecar One") != std::string::npos);
  REQUIRE(waitForPosition(*backend, 2500ms, std::chrono::seconds(30)));

  // Seeking back has to re-arm the cursor: the cues behind the playhead
  // were already queued and are long gone from the FIFO, so without a
  // rescan the first half of the file would stay blank for the rest of
  // the seek.
  REQUIRE(backend->seek(0ms));
  frames = pullSubtitleFrames(ffmpeg, 1, std::chrono::seconds(30));
  backend->stop();
  backend->close();

  REQUIRE(frames.size() >= 1);
  CHECK(frames[0].text.find("Sidecar One") != std::string::npos);
}

TEST_CASE("a forward seek shows the tail of a dense sidecar, in order") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping sidecar forward-seek test");
    return;
  }
  ScratchDir dir;
  const std::string local = dir.copyIn(media, "movie.mkv");
  REQUIRE_FALSE(local.empty());
  // Six cues crammed into the first half second, so seeking past them
  // leaves a whole burst behind the playhead.
  std::string srt;
  for (int i = 1; i <= 6; ++i) {
    srt += std::to_string(i) + "\n00:00:00,000 --> 00:00:00,500\nCue " +
           std::to_string(i) + "\n\n";
  }
  dir.write("movie.srt", srt);
  dir.write("movie.tail.srt", "1\n00:00:03,000 --> 00:00:04,000\nTail\n\n");

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{local}));
  soar::TrackId id = -1;
  REQUIRE(backend->loadExternalSubtitle(dir.file("movie.srt"), id));
  REQUIRE(backend->selectTrack(soar::TrackType::Subtitle, id));
  auto* ffmpeg = static_cast<soar::FFmpegBackend*>(backend.get());

  // The pump is driven by the playhead, so reaching the tail of the file
  // is what makes the skipped burst surface. What comes out is the newest
  // handful in cue order: a forward seek is not a request to replay a
  // hundred cues the user seeked away from.
  REQUIRE(backend->seek(3900ms));
  REQUIRE(backend->play());
  const auto frames = pullSubtitleFrames(ffmpeg, 4, std::chrono::seconds(30));
  backend->stop();
  backend->close();

  CHECK(frames.size() == 4);
  if (frames.size() == 4) {
    CHECK(frames[0].text.find("Cue 3") != std::string::npos);
    CHECK(frames[1].text.find("Cue 4") != std::string::npos);
    CHECK(frames[2].text.find("Cue 5") != std::string::npos);
    CHECK(frames[3].text.find("Cue 6") != std::string::npos);
  }
}

TEST_CASE("disabling subtitles stops the sidecar") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping sidecar disable test");
    return;
  }
  ScratchDir dir;
  const std::string local = dir.copyIn(media, "movie.mkv");
  REQUIRE_FALSE(local.empty());
  dir.write("movie.srt",
            "1\n00:00:00,000 --> 00:00:01,000\nSidecar One\n\n"
            "2\n00:00:01,000 --> 00:00:02,000\nSidecar Two\n\n");

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{local}));
  soar::TrackId id = -1;
  REQUIRE(backend->loadExternalSubtitle(dir.file("movie.srt"), id));
  REQUIRE(backend->selectTrack(soar::TrackType::Subtitle, id));
  // Turning subtitles off has to stop the pump, not just the rendering: a
  // cue that was already due must not surface after the user asked for
  // silence. The track itself stays loaded, so it can be switched back on.
  REQUIRE(backend->disableSubtitles());
  CHECK(backend->mediaInfo().selected_subtitle == -1);

  REQUIRE(backend->play());
  const auto frames = pullSubtitleFrames(
    static_cast<soar::FFmpegBackend*>(backend.get()), 1, std::chrono::seconds(3));
  backend->stop();

  int titled = 0;
  for (const auto& t : backend->mediaInfo().tracks) {
    if (t.title == "movie.srt") ++titled;
  }
  CHECK(titled == 1);
  backend->close();

  CHECK(frames.empty());
}

TEST_CASE("selecting an embedded subtitle track switches the decoder and the surface") {
  // The selection-semantics batch: "selecting" an embedded subtitle
  // stream is decode semantics, not metadata. The dual fixture carries a
  // default subrip stream (Alpha cues) and a second styled ASS stream
  // (one red cue across the whole fixture), so which stream is decoded
  // is observable in both directions — the queue's plain text and the
  // canvas's pixels — and a non-default selection must build a decoder
  // for that stream mid-play, exactly like the audio switch.
  std::string media;
  if (!envMedia("SOAR_TEST_SUBS_DUAL", media)) {
    MESSAGE("SOAR_TEST_SUBS_DUAL not set; skipping subtitle selection test");
    return;
  }
  ScratchDir dir;
  const std::string local = dir.copyIn(media, "movie.mkv");
  REQUIRE_FALSE(local.empty());

  auto backend = soar::makeFFmpegBackend();
  auto* ffmpeg = static_cast<soar::FFmpegBackend*>(backend.get());
#ifdef SOAR_WITH_LIBASS
  auto* ass = backend->assRenderer();
  REQUIRE(ass != nullptr);
  ass->setDefaultFont(SOAR_TEST_FONT_FILE);
#endif
  REQUIRE(backend->open(soar::MediaSource{local}));

  soar::TrackId text_id = -1;
  soar::TrackId ass_id = -1;
  for (const auto& t : backend->mediaInfo().tracks) {
    if (t.type != soar::TrackType::Subtitle) continue;
    if (t.codec == "ass" || t.codec == "ssa") {
      ass_id = t.id;
    } else {
      text_id = t.id;
    }
  }
  REQUIRE(text_id >= 0);
  REQUIRE(ass_id >= 0);
  REQUIRE(ass_id != text_id);

  REQUIRE(backend->play());
#ifdef SOAR_WITH_LIBASS
  // Open state unchanged: the default subrip stream feeds the plain-text
  // queue and the canvas is empty — that is the baseline the switch
  // below has to change.
  const auto plain = pullSubtitleFrames(ffmpeg, 1, std::chrono::seconds(30));
  REQUIRE(contains(plain, "Alpha"));

  // Switch to the styled stream: decoder built mid-play, feed re-armed
  // on that stream's own CodecPrivate, queue flushed. A seek back to the
  // top makes the red cue deterministic — events resume from the read
  // position, the batch-1b no-rescan boundary.
  REQUIRE(backend->selectTrack(soar::TrackType::Subtitle, ass_id));
  CHECK(backend->mediaInfo().selected_subtitle == ass_id);

  soar::AssFrame f;
  bool red = false;
  for (int attempt = 0; attempt < 5 && !red; ++attempt) {
    REQUIRE(backend->seek(std::chrono::milliseconds(0)));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline && !red) {
      red = ass->renderAt(ffmpeg->position().count(), &f) && isAllRed(f);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  REQUIRE(red);

  // While the canvas holds the selection the queue serves nothing: the
  // subrip stream's packets are skipped by the decode gate, so the two
  // sources cannot mix on screen.
  CHECK(pullSubtitleFrames(ffmpeg, 1, std::chrono::milliseconds(600)).empty());

  // "Off" clears both surfaces at once: the canvas drops the red and the
  // queue stays silent through the cues still ahead in both streams.
  REQUIRE(backend->disableSubtitles());
  bool vanished = false;
  const auto vanish_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < vanish_deadline && !vanished) {
    vanished = !ass->renderAt(ffmpeg->position().count(), &f) && f.changed;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK(vanished);
  CHECK(f.rgba.empty());
  CHECK(pullSubtitleFrames(ffmpeg, 1, std::chrono::milliseconds(600)).empty());

  // Back onto the plain stream: an embedded text track is a canvas
  // source once it is the selection — its subrip (ASS-rect) cues render
  // through the synthesized default style.
  REQUIRE(backend->selectTrack(soar::TrackType::Subtitle, text_id));
  bool glyphs = false;
  for (int attempt = 0; attempt < 5 && !glyphs; ++attempt) {
    REQUIRE(backend->seek(std::chrono::milliseconds(0)));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline && !glyphs) {
      glyphs = ass->renderAt(ffmpeg->position().count(), &f) && hasVisible(f) &&
               !isAllRed(f);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  REQUIRE(glyphs);
  CHECK(pullSubtitleFrames(ffmpeg, 1, std::chrono::milliseconds(600)).empty());

  // A selection from Stopped swaps the decoder in directly (no decode
  // thread to hand it to) — the last selection left the text stream
  // installed, so this is a genuine switch, and the replay poll proves
  // the swapped-in decoder actually decodes.
  REQUIRE(backend->stop());
  REQUIRE(backend->selectTrack(soar::TrackType::Subtitle, ass_id));
  REQUIRE(backend->play());
  red = false;
  for (int attempt = 0; attempt < 5 && !red; ++attempt) {
    REQUIRE(backend->seek(std::chrono::milliseconds(0)));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline && !red) {
      red = ass->renderAt(ffmpeg->position().count(), &f) && isAllRed(f);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  REQUIRE(red);
#else
  // No canvas in this build; the selection is still decode semantics —
  // the queue's text says which stream is being decoded, and switching
  // back says the original decoder returned.
  const auto alpha = pullSubtitleFrames(ffmpeg, 1, std::chrono::seconds(30));
  REQUIRE(contains(alpha, "Alpha"));

  REQUIRE(backend->selectTrack(soar::TrackType::Subtitle, ass_id));
  REQUIRE(backend->seek(std::chrono::milliseconds(0)));
  const auto blanket = pullSubtitleFrames(ffmpeg, 1, std::chrono::seconds(30));
  REQUIRE(contains(blanket, "Red Blanket"));

  REQUIRE(backend->selectTrack(soar::TrackType::Subtitle, text_id));
  REQUIRE(backend->seek(std::chrono::milliseconds(0)));
  const auto again = pullSubtitleFrames(ffmpeg, 1, std::chrono::seconds(30));
  REQUIRE(contains(again, "Alpha"));

  // From Stopped the swap is direct (no decode thread running); the
  // replay poll proves the swapped-in decoder decodes.
  REQUIRE(backend->stop());
  REQUIRE(backend->selectTrack(soar::TrackType::Subtitle, ass_id));
  REQUIRE(backend->play());
  REQUIRE(backend->seek(std::chrono::milliseconds(0)));
  const auto replay = pullSubtitleFrames(ffmpeg, 1, std::chrono::seconds(30));
  REQUIRE(contains(replay, "Red Blanket"));
#endif
  backend->stop();
  backend->close();
}

TEST_CASE("a subtitle selection after natural EOF joins the dead decode thread") {
  // The third owner of subtitle_decoder_: when the decode thread has run
  // out of data at EOF it is no longer running but still joinable, and a
  // selection arriving then must join it before swapping the decoder in
  // directly (no pending handover — there is nobody to hand it to), the
  // audio-switch twin's contract. The Ended -> Stopped flip the join
  // causes is part of the observable surface.
  std::string media;
  if (!envMedia("SOAR_TEST_SUBS_DUAL", media)) {
    MESSAGE("SOAR_TEST_SUBS_DUAL not set; skipping post-EOF subtitle selection test");
    return;
  }

  auto backend = soar::makeFFmpegBackend();
  auto* ffmpeg = static_cast<soar::FFmpegBackend*>(backend.get());
#ifdef SOAR_WITH_LIBASS
  auto* ass = backend->assRenderer();
  REQUIRE(ass != nullptr);
  ass->setDefaultFont(SOAR_TEST_FONT_FILE);
#endif
  REQUIRE(backend->open(soar::MediaSource{media}));

  soar::TrackId ass_id = -1;
  for (const auto& t : backend->mediaInfo().tracks) {
    if (t.type == soar::TrackType::Subtitle &&
        (t.codec == "ass" || t.codec == "ssa")) {
      ass_id = t.id;
    }
  }
  REQUIRE(ass_id >= 0);

  REQUIRE(backend->play());
  bool ended = false;
  for (int i = 0; i < 120 && !ended; ++i) {
    ended = backend->state() == soar::PlaybackState::Ended;
    if (!ended) {
      std::this_thread::sleep_for(100ms);
    }
  }
  REQUIRE(ended);

  // The direct swap (vs the pending handover of the playing case above)
  // and the Stopped report the join produces.
  REQUIRE(backend->selectTrack(soar::TrackType::Subtitle, ass_id));
  CHECK(backend->mediaInfo().selected_subtitle == ass_id);
  CHECK(backend->state() == soar::PlaybackState::Stopped);
  CHECK(backend->lastError().empty());

  // The swapped-in decoder actually decodes: replay from the top (events
  // resume from the read position, so the seek makes cue one
  // deterministic — the batch-1b boundary, same retry shape as above).
  REQUIRE(backend->play());
#ifdef SOAR_WITH_LIBASS
  soar::AssFrame f;
  bool red = false;
  for (int attempt = 0; attempt < 5 && !red; ++attempt) {
    REQUIRE(backend->seek(std::chrono::milliseconds(0)));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline && !red) {
      red = ass->renderAt(ffmpeg->position().count(), &f) && isAllRed(f);
      std::this_thread::sleep_for(20ms);
    }
  }
  REQUIRE(red);
#else
  REQUIRE(backend->seek(std::chrono::milliseconds(0)));
  const auto replay = pullSubtitleFrames(ffmpeg, 1, std::chrono::seconds(30));
  REQUIRE(contains(replay, "Red Blanket"));
#endif
  backend->stop();
  backend->close();
}

TEST_CASE("a subtitle selection while paused hands over on the parked thread") {
  // The paused arm of the running-thread check: under Paused the decode
  // thread is parked on the condition variable, and the selection must
  // still reach it — the pending slot plus the notify wake the parked
  // loop (its wait predicate counts a pending subtitle switch), which
  // applies the swap without leaving the paused state. The audio switch
  // has the same paused twin; this pins the subtitle side of that arm.
  std::string media;
  if (!envMedia("SOAR_TEST_SUBS_DUAL", media)) {
    MESSAGE("SOAR_TEST_SUBS_DUAL not set; skipping paused subtitle selection test");
    return;
  }

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{media}));

  soar::TrackId ass_id = -1;
  for (const auto& t : backend->mediaInfo().tracks) {
    if (t.type == soar::TrackType::Subtitle &&
        (t.codec == "ass" || t.codec == "ssa")) {
      ass_id = t.id;
    }
  }
  REQUIRE(ass_id >= 0);

  REQUIRE(backend->play());
  bool started = false;
  for (int i = 0; i < 200 && !started; ++i) {
    started = backend->position() > 0ms;
    if (!started) {
      std::this_thread::sleep_for(10ms);
    }
  }
  REQUIRE(started);

  REQUIRE(backend->pause());
  const auto frozen = backend->position();

  // The selection lands while paused: the parked decode thread consumes
  // the pending handover and reopens the decode gate, all without
  // disturbing the clock.
  REQUIRE(backend->selectTrack(soar::TrackType::Subtitle, ass_id));
  CHECK(backend->mediaInfo().selected_subtitle == ass_id);
  CHECK(backend->state() == soar::PlaybackState::Paused);
  CHECK(backend->lastError().empty());
  std::this_thread::sleep_for(100ms);
  CHECK(backend->position() == frozen);

  // And playback resumes from where it was, onto the switched stream.
  REQUIRE(backend->play());
  CHECK(backend->state() == soar::PlaybackState::Playing);

  backend->stop();
  backend->close();
}

TEST_CASE("a selected mov_text stream renders on the canvas") {
  // The last plain-text-only source joins the canvas. Which rect arm
  // serves it depends on the decoder's rect shape, and that varies by
  // FFmpeg version: locally (FFmpeg 8) mov_text wraps its cues in ASS
  // rects, so the verbatim-ass feed arm carries the canvas and the
  // raw-text rebuild arm stays quiet; the assertions are therefore
  // shape-agnostic. Selecting the embedded stream re-arms the feed with
  // a synthesized default header either way (mov_text is not an ASS/SSA
  // codec, so there is no CodecPrivate to feed). The libass build proves
  // the canvas (glyphs after selection) and that the queue stays empty —
  // no double render; the stub build proves the selection still governs
  // decoding through the queue. Selecting the embedded track from under
  // a held document also exercises the document release.
  std::string media;
  if (!envMedia("SOAR_TEST_MOVTEXT_MEDIA", media)) {
    MESSAGE("SOAR_TEST_MOVTEXT_MEDIA not set; skipping mov_text selection test");
    return;
  }
  ScratchDir dir;
  const std::string local = dir.copyIn(media, "movie.mp4");
  REQUIRE_FALSE(local.empty());
  dir.write("movie.srt",
            "1\n00:00:00,000 --> 00:00:06,000\nSidecar Detour\n\n");

  auto backend = soar::makeFFmpegBackend();
  auto* ffmpeg = static_cast<soar::FFmpegBackend*>(backend.get());
  soar::TrackId embedded_id = -1;
#ifdef SOAR_WITH_LIBASS
  auto* ass = backend->assRenderer();
  REQUIRE(ass != nullptr);
  ass->setDefaultFont(SOAR_TEST_FONT_FILE);
#endif
  REQUIRE(backend->open(soar::MediaSource{local}));
  for (const auto& t : backend->mediaInfo().tracks) {
    if (t.type == soar::TrackType::Subtitle &&
        (embedded_id < 0 || t.id < embedded_id)) {
      embedded_id = t.id;
    }
  }
  REQUIRE(embedded_id >= 0);

  // Unselected open state: the embedded text stream decodes to the
  // plain-text queue, unchanged by this batch.
  REQUIRE(backend->play());
  const auto hello = pullSubtitleFrames(ffmpeg, 1, std::chrono::seconds(30));
  REQUIRE(contains(hello, "Hello"));

#ifdef SOAR_WITH_LIBASS
  // Detour through a document, then select the embedded text stream: the
  // document must be released and the canvas must take the stream.
  soar::TrackId ext = -1;
  REQUIRE(backend->loadExternalSubtitle(dir.file("movie.srt"), ext));
  REQUIRE(backend->selectTrack(soar::TrackType::Subtitle, ext));
  CHECK(ffmpeg->subtitleDocumentActive());

  REQUIRE(backend->selectTrack(soar::TrackType::Subtitle, embedded_id));
  CHECK_FALSE(ffmpeg->subtitleDocumentActive());

  soar::AssFrame f;
  bool glyphs = false;
  for (int attempt = 0; attempt < 5 && !glyphs; ++attempt) {
    REQUIRE(backend->seek(std::chrono::milliseconds(0)));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline && !glyphs) {
      glyphs = ass->renderAt(ffmpeg->position().count(), &f) && hasVisible(f);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  REQUIRE(glyphs);
  // The feed owns the frames now; the plain-text queue stays empty.
  CHECK(pullSubtitleFrames(ffmpeg, 1, std::chrono::milliseconds(600)).empty());
#else
  // No canvas: selecting the same stream re-opens the queue path through
  // it (gate on, no decoder switch needed — it is the stream open built).
  REQUIRE(backend->selectTrack(soar::TrackType::Subtitle, embedded_id));
  REQUIRE(backend->seek(std::chrono::milliseconds(0)));
  const auto again = pullSubtitleFrames(ffmpeg, 1, std::chrono::seconds(30));
  REQUIRE(contains(again, "Hello"));
#endif
  backend->stop();
  backend->close();
}

TEST_CASE("selecting a sidecar closes the embedded decode gate") {
  // The batch-1b/1c overlay — an embedded stream and a selected sidecar
  // both reaching the screen — closes at the source now: selecting any
  // sidecar stops the embedded packets, so its words never reach the
  // queue again. No canvas is needed for the fix to hold, so the same
  // assertion pins both builds (the libass build already hid the mix
  // behind subtitleDocumentActive; the queue is the deeper proof).
  std::string media;
  if (!envMedia("SOAR_TEST_MOVTEXT_MEDIA", media)) {
    MESSAGE("SOAR_TEST_MOVTEXT_MEDIA not set; skipping gate test");
    return;
  }
  ScratchDir dir;
  const std::string local = dir.copyIn(media, "movie.mp4");
  REQUIRE_FALSE(local.empty());
  dir.write("movie.srt",
            "1\n00:00:00,000 --> 00:00:06,000\nOverlay Wins\n\n");

  auto backend = soar::makeFFmpegBackend();
  auto* ffmpeg = static_cast<soar::FFmpegBackend*>(backend.get());
  REQUIRE(backend->open(soar::MediaSource{local}));

  // Baseline: the embedded mov_text stream decodes while unselected —
  // its words arrive without any selection.
  REQUIRE(backend->play());
  const auto embedded = pullSubtitleFrames(ffmpeg, 1, std::chrono::seconds(30));
  REQUIRE(contains(embedded, "Hello"));

  soar::TrackId ext = -1;
  REQUIRE(backend->loadExternalSubtitle(dir.file("movie.srt"), ext));
  REQUIRE(backend->selectTrack(soar::TrackType::Subtitle, ext));

  // The sidecar's own cues pump through; the embedded stream's words are
  // gone from the queue in both directions — the current cue was flushed
  // and the ones still ahead can no longer arrive.
  const auto mixed = pullSubtitleFrames(ffmpeg, 1, std::chrono::seconds(30));
  CHECK(contains(mixed, "Overlay Wins"));
  CHECK_FALSE(contains(mixed, "Hello"));
  CHECK_FALSE(contains(mixed, "World"));
  CHECK_FALSE(contains(mixed, "Two"));
  CHECK_FALSE(contains(mixed, "Lines"));
  backend->stop();
  backend->close();
}

TEST_CASE("disabling subtitles silences the embedded stream at once") {
  // "Off" is decode semantics now, not metadata: the embedded stream's
  // packets stop being processed and the frames it already queued are
  // dropped, so the cue on screen disappears at the moment of the switch
  // instead of wearing out its own duration.
  std::string media;
  if (!envMedia("SOAR_TEST_SUBS_MEDIA", media)) {
    MESSAGE("SOAR_TEST_SUBS_MEDIA not set; skipping embedded disable test");
    return;
  }
  ScratchDir dir;
  const std::string local = dir.copyIn(media, "movie.mkv");
  REQUIRE_FALSE(local.empty());

  auto backend = soar::makeFFmpegBackend();
  auto* ffmpeg = static_cast<soar::FFmpegBackend*>(backend.get());
  REQUIRE(backend->open(soar::MediaSource{local}));
  REQUIRE(backend->play());
  const auto frames = pullSubtitleFrames(ffmpeg, 1, std::chrono::seconds(30));
  REQUIRE(contains(frames, "Hello"));

  REQUIRE(backend->disableSubtitles());
  // Nothing further arrives: neither the cue that was current at the
  // switch (flushed) nor World/Two Lines still ahead in the stream
  // (gated off at the decoder).
  CHECK(pullSubtitleFrames(ffmpeg, 1, std::chrono::milliseconds(1500)).empty());
  backend->stop();
  backend->close();
}

TEST_CASE("switching to a broken non-default subtitle stream fails cleanly") {
  // The §4 pattern: open builds a decoder for the default subtitle
  // stream only, so a container whose second subtitle track carries an
  // unknown CodecID still opens. The damage surfaces at selection time —
  // a failed switch that leaves the default stream decoding.
  std::string media;
  if (!envMedia("SOAR_TEST_SUBS_DUAL", media)) {
    MESSAGE("SOAR_TEST_SUBS_DUAL not set; skipping broken subtitle switch test");
    return;
  }
  ScratchDir dir;
  // Same-length CodecID patch on a copy: S_TEXT/ASS -> S_TEXT/XXX. The
  // EBML sizes stay valid and the demuxer maps the unknown id to
  // AV_CODEC_ID_NONE, which is exactly the "no decoder for this track"
  // selection-time failure.
  std::string bytes;
  {
    std::ifstream in(media, std::ios::binary);
    REQUIRE(in);
    std::ostringstream ss;
    ss << in.rdbuf();
    bytes = ss.str();
  }
  const std::size_t codec_id = bytes.find("S_TEXT/ASS");
  REQUIRE(codec_id != std::string::npos);
  bytes.replace(codec_id, 10, "S_TEXT/XXX");
  dir.write("broken.mkv", bytes);
  const std::string local = dir.file("broken.mkv");
  REQUIRE_FALSE(local.empty());

  auto backend = soar::makeFFmpegBackend();
  auto* ffmpeg = static_cast<soar::FFmpegBackend*>(backend.get());
  REQUIRE(backend->open(soar::MediaSource{local}));

  // The default subrip stream is healthy; the other subtitle track is
  // the broken one. Track codecs carry the decoder's registered name —
  // libavcodec calls the SubRip decoder "srt" (ffprobe's "subrip" is the
  // codec descriptor's name) — and the patched stream reports the
  // unknown-codec placeholder instead of "ass".
  soar::TrackId text_id = -1;
  soar::TrackId broken_id = -1;
  for (const auto& t : backend->mediaInfo().tracks) {
    if (t.type != soar::TrackType::Subtitle) continue;
    if (t.codec == "srt") {
      text_id = t.id;
    } else {
      broken_id = t.id;
    }
  }
  REQUIRE(text_id >= 0);
  REQUIRE(broken_id >= 0);

  REQUIRE(backend->play());
  const auto alpha = pullSubtitleFrames(ffmpeg, 1, std::chrono::seconds(30));
  REQUIRE(contains(alpha, "Alpha"));

  CHECK_FALSE(backend->selectTrack(soar::TrackType::Subtitle, broken_id));
  CHECK(backend->lastError().find("subtitle") != std::string::npos);
  CHECK(backend->mediaInfo().selected_subtitle == -1);

  // The failed switch changed nothing: the default stream still decodes.
  REQUIRE(backend->seek(std::chrono::milliseconds(0)));
  const auto again = pullSubtitleFrames(ffmpeg, 1, std::chrono::seconds(30));
  REQUIRE(contains(again, "Alpha"));
  backend->stop();
  backend->close();
}

TEST_CASE("a container whose default subtitle stream has no decoder fails to open") {
  // The §4 pattern inverted: open builds a decoder for the default
  // subtitle stream, so patching *that* stream's CodecID surfaces at
  // open time — the complement of the select-time failure above, where
  // only the non-default track was broken.
  std::string media;
  if (!envMedia("SOAR_TEST_SUBS_DUAL", media)) {
    MESSAGE("SOAR_TEST_SUBS_DUAL not set; skipping broken default subtitle test");
    return;
  }
  ScratchDir dir;
  std::string bytes;
  {
    std::ifstream in(media, std::ios::binary);
    REQUIRE(in);
    std::ostringstream ss;
    ss << in.rdbuf();
    bytes = ss.str();
  }
  // Same-length CodecID patch on the default subrip stream:
  // S_TEXT/UTF8 -> S_TEXT/QQQQ (both 11 bytes, so every EBML size in the
  // header stays valid). The demuxer maps the unknown id to
  // AV_CODEC_ID_NONE and decoder setup has no decoder for it, so open
  // fails loudly instead of handing out a playable-looking MediaInfo.
  const std::size_t codec_id = bytes.find("S_TEXT/UTF8");
  REQUIRE(codec_id != std::string::npos);
  bytes.replace(codec_id, 11, "S_TEXT/QQQQ");
  dir.write("broken_default.mkv", bytes);
  const std::string local = dir.file("broken_default.mkv");
  REQUIRE_FALSE(local.empty());

  CountingSink sink;
  auto backend = soar::makeFFmpegBackend();
  backend->setEventSink(&sink);

  CHECK_FALSE(backend->open(soar::MediaSource{local}));
  CHECK(backend->lastError().find("subtitle codec not found") != std::string::npos);
  CHECK(backend->state() == soar::PlaybackState::Error);
  CHECK(sink.errors.load() >= 1);

  // The backend recovers and opens the healthy media afterwards.
  REQUIRE(backend->open(soar::MediaSource{media}));
  CHECK(backend->state() == soar::PlaybackState::Stopped);
  backend->close();
}

TEST_CASE("exporting embedded subtitle tracks yields SubRip text") {
  // The export pass (backend.h exportSubtitleText): an independent reopen
  // of the same source, every cue on that stream decoded, SubRip assembly.
  // The dual fixture carries both rect shapes in one container — the
  // default subrip stream and the styled ASS stream — plus non-subtitle
  // stream ids that must share the "not an embedded subtitle track"
  // refusal with out-of-range ids.
  std::string media;
  if (!envMedia("SOAR_TEST_SUBS_DUAL", media)) {
    MESSAGE("SOAR_TEST_SUBS_DUAL not set; skipping subtitle export test");
    return;
  }
  ScratchDir dir;
  const std::string local = dir.copyIn(media, "movie.mkv");
  REQUIRE_FALSE(local.empty());

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{local}));

  soar::TrackId text_id = -1;
  soar::TrackId ass_id = -1;
  soar::TrackId video_id = -1;
  for (const auto& t : backend->mediaInfo().tracks) {
    if (t.type == soar::TrackType::Subtitle) {
      if (t.codec == "ass" || t.codec == "ssa") {
        ass_id = t.id;
      } else {
        text_id = t.id;
      }
    } else if (video_id < 0) {
      video_id = t.id;
    }
  }
  REQUIRE(text_id >= 0);
  REQUIRE(ass_id >= 0);
  REQUIRE(video_id >= 0);

  // The subrip stream: a full SubRip block with the fixture's own timing.
  std::string srt;
  REQUIRE(backend->exportSubtitleText(text_id, srt));
  CHECK(srt.find("1\n00:00:00,000 --> 00:00:02,000\nAlpha One") !=
        std::string::npos);
  CHECK(srt.find("Alpha Two") != std::string::npos);

  // The ASS stream: rects arrive as ass and go through assDialogueText.
  std::string ass_srt;
  REQUIRE(backend->exportSubtitleText(ass_id, ass_srt));
  CHECK(ass_srt.find("Red Blanket") != std::string::npos);
  CHECK(ass_srt.find("--> 00:00:04,000") != std::string::npos);

  // Refusals: a non-subtitle id and ids outside the stream table share
  // one answer, and the output never carries a previous export's text.
  std::string nope;
  CHECK_FALSE(backend->exportSubtitleText(video_id, nope));
  CHECK(nope.empty());
  CHECK(backend->lastError().find("not an embedded subtitle track") !=
        std::string::npos);
  CHECK_FALSE(backend->exportSubtitleText(-1, nope));
  CHECK(nope.empty());
  CHECK_FALSE(backend->exportSubtitleText(99, nope));
  CHECK(nope.empty());
  backend->close();
}

TEST_CASE("export fails without media and when the source vanishes") {
  // Two failure legs of the export pass that need no broken bytes: the
  // unopened guard, and the reopen — the pass has its own input, so
  // deleting the file behind the open handle breaks only the export.
  auto fresh = soar::makeFFmpegBackend();
  std::string srt;
  CHECK_FALSE(fresh->exportSubtitleText(0, srt));
  CHECK(srt.empty());
  CHECK(fresh->lastError().find("no media opened") != std::string::npos);
  fresh->close();

  std::string media;
  if (!envMedia("SOAR_TEST_SUBS_DUAL", media)) {
    MESSAGE("SOAR_TEST_SUBS_DUAL not set; skipping vanished-source export test");
    return;
  }
  ScratchDir dir;
  const std::string local = dir.copyIn(media, "movie.mkv");
  REQUIRE_FALSE(local.empty());
  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{local}));
  soar::TrackId text_id = -1;
  for (const auto& t : backend->mediaInfo().tracks) {
    if (t.type == soar::TrackType::Subtitle && t.codec != "ass" &&
        t.codec != "ssa") {
      text_id = t.id;
      break;
    }
  }
  REQUIRE(text_id >= 0);
  std::error_code ec;
  std::filesystem::remove(local, ec);
  REQUIRE_FALSE(ec);

  CHECK_FALSE(backend->exportSubtitleText(text_id, srt));
  CHECK(srt.empty());
  CHECK(backend->lastError().find("cannot reopen") != std::string::npos);
  backend->close();
}

TEST_CASE("exporting an unknown-codec subtitle track fails at the reopen probe") {
  // The §4 pattern at the export boundary: same-length CodecID patch
  // S_TEXT/ASS -> S_TEXT/XXX (EBML sizes stay valid). The track still
  // reports as a subtitle stream, so the embedded check passes, but the
  // reopened input carries codec id NONE: the header probe does not invent
  // one and the pass fails with its reopen-side message.
  std::string media;
  if (!envMedia("SOAR_TEST_SUBS_DUAL", media)) {
    MESSAGE("SOAR_TEST_SUBS_DUAL not set; skipping broken-codec export test");
    return;
  }
  ScratchDir dir;
  std::string bytes;
  {
    std::ifstream in(media, std::ios::binary);
    REQUIRE(in);
    std::ostringstream ss;
    ss << in.rdbuf();
    bytes = ss.str();
  }
  const std::size_t codec_id = bytes.find("S_TEXT/ASS");
  REQUIRE(codec_id != std::string::npos);
  bytes.replace(codec_id, 10, "S_TEXT/XXX");
  dir.write("broken.mkv", bytes);
  const std::string local = dir.file("broken.mkv");
  REQUIRE_FALSE(local.empty());

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{local}));
  soar::TrackId broken_id = -1;
  for (const auto& t : backend->mediaInfo().tracks) {
    if (t.type == soar::TrackType::Subtitle && t.codec != "srt") {
      broken_id = t.id;
      break;
    }
  }
  REQUIRE(broken_id >= 0);

  std::string srt;
  CHECK_FALSE(backend->exportSubtitleText(broken_id, srt));
  CHECK(srt.empty());
  CHECK(backend->lastError().find("exportSubtitleText") != std::string::npos);
  backend->close();
}

TEST_CASE("exporting a track that yields no text fails with the empty-track answer") {
  // Every payload stripped to nothing: patch the visible cue of the
  // empty-payload fixture to another override-only string of the same
  // length (7 bytes in place — EBML sizes untouched). Both events then
  // decode to empty text, the loop emits no cue, and no earlier error was
  // set, so the pass reports the cue_count==0 answer.
  std::string media;
  if (!envMedia("SOAR_TEST_SUBS_EMPTY", media)) {
    MESSAGE("SOAR_TEST_SUBS_EMPTY not set; skipping empty-cue export test");
    return;
  }
  ScratchDir dir;
  std::string bytes;
  {
    std::ifstream in(media, std::ios::binary);
    REQUIRE(in);
    std::ostringstream ss;
    ss << in.rdbuf();
    bytes = ss.str();
  }
  const std::size_t visible = bytes.find("Visible");
  REQUIRE(visible != std::string::npos);
  bytes.replace(visible, 7, " {\\i1} ");  // strips to nothing after braces
  dir.write("empty_only.mkv", bytes);
  const std::string local = dir.file("empty_only.mkv");
  REQUIRE_FALSE(local.empty());

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{local}));
  soar::TrackId sub_id = -1;
  for (const auto& t : backend->mediaInfo().tracks) {
    if (t.type == soar::TrackType::Subtitle) {
      sub_id = t.id;
      break;
    }
  }
  REQUIRE(sub_id >= 0);

  std::string srt;
  CHECK_FALSE(backend->exportSubtitleText(sub_id, srt));
  CHECK(srt.empty());
  CHECK(backend->lastError().find("no text cues") != std::string::npos);
  backend->close();
}

TEST_CASE("exporting a mov_text track yields the cues regardless of rect shape") {
  // mov_text is the one fixture whose rect shape varies by FFmpeg build
  // (raw per-line text rects on older builds, ass-wrapped on newer), so
  // the assertions stay on content: the same three cues as the srt
  // source, through whichever arm the decoder feeds.
  std::string media;
  if (!envMedia("SOAR_TEST_MOVTEXT_MEDIA", media)) {
    MESSAGE("SOAR_TEST_MOVTEXT_MEDIA not set; skipping mov_text export test");
    return;
  }
  ScratchDir dir;
  const std::string local = dir.copyIn(media, "movie.mp4");
  REQUIRE_FALSE(local.empty());

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{local}));
  soar::TrackId sub_id = -1;
  for (const auto& t : backend->mediaInfo().tracks) {
    if (t.type == soar::TrackType::Subtitle) {
      sub_id = t.id;
      break;
    }
  }
  REQUIRE(sub_id >= 0);

  std::string srt;
  REQUIRE(backend->exportSubtitleText(sub_id, srt));
  CHECK(srt.find("Hello") != std::string::npos);
  CHECK(srt.find("World") != std::string::npos);
  CHECK(srt.find("Two") != std::string::npos);
  CHECK(srt.find("Lines") != std::string::npos);
  CHECK(srt.find(" --> ") != std::string::npos);
  backend->close();
}

TEST_CASE("exportSubtitleText exercises the duration fallback to kDefaultCueDuration") {
  // The export pass falls back to kDefaultCueDuration when both the
  // ASS end_display_time and the packet duration are missing or invalid.
  // The subs_media.mkv fixture (SRT in MKV) produces packets where the
  // srt decoder leaves packet duration at 0/AV_NOPTS_VALUE, triggering
  // the fallback at line 1867.
  std::string media;
  if (!envMedia("SOAR_TEST_SUBS_MEDIA", media)) {
    MESSAGE("SOAR_TEST_SUBS_MEDIA not set; skipping duration fallback export test");
    return;
  }
  ScratchDir dir;
  const std::string local = dir.copyIn(media, "movie.mkv");
  REQUIRE_FALSE(local.empty());

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{local}));

  soar::TrackId text_id = -1;
  for (const auto& t : backend->mediaInfo().tracks) {
    if (t.type == soar::TrackType::Subtitle && t.codec != "ass" &&
        t.codec != "ssa") {
      text_id = t.id;
      break;
    }
  }
  REQUIRE(text_id >= 0);

  std::string srt;
  REQUIRE(backend->exportSubtitleText(text_id, srt));
  // The fixture has 3 cues: "Hello", "World", "Two\nLines"
  CHECK(srt.find("Hello") != std::string::npos);
  CHECK(srt.find("World") != std::string::npos);
  CHECK(srt.find("Two") != std::string::npos);
  CHECK(srt.find("Lines") != std::string::npos);
  // Verify timestamp format includes the fallback duration path.
  CHECK(srt.find(" --> ") != std::string::npos);
  backend->close();
}

TEST_CASE("exportSubtitleText handles decode failure (got_sub == 0) gracefully") {
  // When avcodec_decode_subtitle2 returns got_sub == 0 (no output for a
  // valid packet), the pass must skip that packet and continue decoding
  // subsequent ones instead of aborting the whole export.
  // The subs_junk_media.mkv fixture contains cues with empty/override-only
  // payloads that decode to got_sub == 0 or empty text, plus valid cues.
  std::string media;
  if (!envMedia("SOAR_TEST_JUNK_SRT_MEDIA", media)) {
    MESSAGE("SOAR_TEST_JUNK_SRT_MEDIA not set; skipping decode-failure export test");
    return;
  }
  ScratchDir dir;
  const std::string local = dir.copyIn(media, "movie.mkv");
  REQUIRE_FALSE(local.empty());

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{local}));

  soar::TrackId text_id = -1;
  for (const auto& t : backend->mediaInfo().tracks) {
    if (t.type == soar::TrackType::Subtitle && t.codec != "ass" &&
        t.codec != "ssa") {
      text_id = t.id;
      break;
    }
  }
  REQUIRE(text_id >= 0);

  std::string srt;
  // The export should succeed (it finds valid cues among the junk) and
  // produce the valid ones, skipping the decode-failure packets.
  // The junk fixture has cues like "Hard\nbreak", "Trailing backslash", etc.
  REQUIRE(backend->exportSubtitleText(text_id, srt));
  CHECK(srt.find("Hard") != std::string::npos);
  CHECK(srt.find("Trailing") != std::string::npos);
  backend->close();
}

TEST_CASE("exportSubtitleText covers both SUBTITLE_TEXT and SUBTITLE_ASS rect branches in one pass") {
  // The dual subtitle fixture carries both a subrip stream (SUBTITLE_TEXT
  // rects) and an ASS stream (SUBTITLE_ASS rects via assDialogueText).
  // Running export on both streams in one test ensures both rect-type
  // branches are hit in a single coverage run, which helps gcov's branch
  // tracking for the if/else-if chain at lines 1880-1883.
  std::string media;
  if (!envMedia("SOAR_TEST_SUBS_DUAL", media)) {
    MESSAGE("SOAR_TEST_SUBS_DUAL not set; skipping dual-rect export test");
    return;
  }
  ScratchDir dir;
  const std::string local = dir.copyIn(media, "movie.mkv");
  REQUIRE_FALSE(local.empty());

  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{local}));

  soar::TrackId text_id = -1;
  soar::TrackId ass_id = -1;
  for (const auto& t : backend->mediaInfo().tracks) {
    if (t.type == soar::TrackType::Subtitle) {
      if (t.codec == "ass" || t.codec == "ssa") {
        ass_id = t.id;
      } else {
        text_id = t.id;
      }
    }
  }
  REQUIRE(text_id >= 0);
  REQUIRE(ass_id >= 0);

  std::string srt;
  REQUIRE(backend->exportSubtitleText(text_id, srt));
  CHECK(srt.find("Alpha One") != std::string::npos);
  CHECK(srt.find("Alpha Two") != std::string::npos);

  std::string ass_srt;
  REQUIRE(backend->exportSubtitleText(ass_id, ass_srt));
  CHECK(ass_srt.find("Red Blanket") != std::string::npos);
  CHECK(ass_srt.find("--> 00:00:04,000") != std::string::npos);

  backend->close();
}

TEST_CASE("a local source with a cache_dir ignores the cache") {
  // backend.h contract: MediaSource::cache_dir arms the disk cache only
  // for http:// URLs and is ignored for local paths. Open a local file
  // with a fresh cache dir set: the direct path must be taken, so no
  // HttpCache is constructed and the directory stays untouched.
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping local cache_dir test");
    return;
  }
  ScratchDir dir;
  auto backend = soar::makeFFmpegBackend();
  REQUIRE(backend->open(soar::MediaSource{media, dir.path}));
  CHECK(backend->state() == soar::PlaybackState::Stopped);
  CHECK(std::filesystem::is_empty(dir.path));
  backend->close();
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

  // 512 bytes: which stage refuses the cut is version behavior (6.1
  // rejects it at the demuxer's own open, 8 accepts that and fails only
  // at find_stream_info), so nothing about the error text is pinned:
  // the contract is just that the open path must fail into the Error
  // state.
  CHECK_FALSE(backend->open(soar::MediaSource{mid}));
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

TEST_CASE("an audio codec with no FFmpeg decoder fails to open") {
  std::string unknown;
  if (!envMedia("SOAR_TEST_UNKNOWN_AUDIO", unknown)) {
    MESSAGE("SOAR_TEST_UNKNOWN_AUDIO not set; skipping unknown-audio test");
    return;
  }

  CountingSink sink;
  auto backend = soar::makeFFmpegBackend();
  backend->setEventSink(&sink);

  // The video stream is healthy; only the audio track's codec id has no
  // decoder. Setup must fail on the audio side, not silently drop it.
  CHECK_FALSE(backend->open(soar::MediaSource{unknown}));
  CHECK(backend->lastError().find("audio codec not found") != std::string::npos);
  CHECK(backend->state() == soar::PlaybackState::Error);
  CHECK(sink.errors.load() >= 1);

  std::string media;
  REQUIRE(mediaAvailable(media));
  REQUIRE(backend->open(soar::MediaSource{media}));
  CHECK(backend->state() == soar::PlaybackState::Stopped);
  backend->close();
}

TEST_CASE("switching to an audio track with no decoder fails without breaking the backend") {
  std::string dual;
  if (!envMedia("SOAR_TEST_UNKNOWN_AUDIO_DUAL", dual)) {
    MESSAGE("SOAR_TEST_UNKNOWN_AUDIO_DUAL not set; skipping dual unknown-audio test");
    return;
  }

  auto backend = soar::makeFFmpegBackend();
  // Open succeeds here even though one track is undecodable: the
  // backend builds a decoder for the selected audio track only, and
  // that track is healthy AAC. The broken A_XXX track only bites when
  // someone selects it explicitly.
  REQUIRE(backend->open(soar::MediaSource{dual}));

  const auto info = backend->mediaInfo();
  std::vector<soar::TrackId> audio_ids;
  for (const auto& track : info.tracks) {
    if (track.type == soar::TrackType::Audio) {
      audio_ids.push_back(track.id);
    }
  }
  REQUIRE(audio_ids.size() >= 2);
  const auto good_id = audio_ids[0];
  const auto bad_id = audio_ids[1];

  // The failed switch reports through lastError and keeps the old
  // decoder installed - the contract differs from the open-time
  // failure above (Error state + event), which is why this is not
  // folded into that case.
  CHECK_FALSE(backend->selectTrack(soar::TrackType::Audio, bad_id));
  CHECK(backend->lastError().find("audio codec not found") != std::string::npos);

  // The failed switch must not poison the backend: the healthy track
  // is still selectable and playback with it still works.
  CHECK(backend->selectTrack(soar::TrackType::Audio, good_id));
  REQUIRE(backend->play());
  std::this_thread::sleep_for(200ms);
  CHECK(backend->state() == soar::PlaybackState::Playing);

  backend->stop();
  backend->close();
}

TEST_CASE("seeking to the exact duration ends a stopped stream; seeking back revives it") {
  std::string media;
  if (!mediaAvailable(media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping the seek-endedness test");
    return;
  }

  CountingSink sink;
  auto backend = soar::makeFFmpegBackend();
  backend->setEventSink(&sink);

  REQUIRE(backend->open(soar::MediaSource{media}));
  // Deliberately not playing: with no decode thread running the seek
  // runs synchronously on the caller's thread, which is the path that
  // re-announces state on an ended-ness flip. A playing stream hands
  // the seek to the decode thread instead and announces from there.
  CHECK(backend->state() == soar::PlaybackState::Stopped);

  // Seeking exactly to the duration flips the stream to Ended...
  const auto duration = backend->mediaInfo().duration;
  CHECK(backend->seek(duration));
  CHECK(backend->state() == soar::PlaybackState::Ended);

  // ...and seeking away from Ended rewinds it back to Paused (press
  // play to resume). Both edges share the seek-path re-announcement.
  CHECK(backend->seek(std::chrono::milliseconds(0)));
  CHECK(backend->state() == soar::PlaybackState::Paused);

  backend->stop();
  backend->close();
}

TEST_CASE("undecodable audio payload ends deterministically without hanging") {
  std::string reject;
  if (!envMedia("SOAR_TEST_AUDIO_REJECT", reject)) {
    MESSAGE("SOAR_TEST_AUDIO_REJECT not set; skipping audio-reject test");
    return;
  }

  CountingSink sink;
  auto backend = soar::makeFFmpegBackend();
  backend->setEventSink(&sink);

  // The container opens and the DTS decoder exists, but it rejects every
  // AAC packet on its sync word. Whether FFmpeg rejects at send or at
  // receive is version behavior (see the video twin), so the contract is
  // the same: playback reaches a terminal state, never hangs, and the
  // backend stays reusable.
  REQUIRE(backend->open(soar::MediaSource{reject}));
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

  CHECK(backend->stop());
  CHECK(backend->state() == soar::PlaybackState::Stopped);
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
  // Eight times real time: the decode thread is no longer throttled to
  // the wall clock, so the whole 3-second stream is decoded as fast as
  // the machine can manage. Without this, builds with heavy sanitizer
  // or coverage instrumentation lose the race against real time and
  // never reach the later segments - the assertion then depends on the
  // runner's speed instead of the converter's behavior.
  REQUIRE(backend->setRate(8.0));
  REQUIRE(backend->play());

  // The stream is three 1-second h264 segments whose SPS changes
  // mid-flight: 160x120 yuv422p, then 320x240 yuv420p, then 480x360
  // yuv422p. The converter must rebuild for every parameter change -
  // including the second rebuild, which frees the destination frame
  // allocated for segment one - instead of stalling or handing out
  // stale-size frames. The 420p middle segment also covers the
  // pass-through path between the two rebuilds.
  //
  // tryGetVideoFrame is a latest-frame-wins mailbox, by design: the UI
  // renders at its own cadence and only wants the newest frame, and a
  // frame the consumer does not sample before the next promotion is
  // gone. At rate 8 the promotion ceiling is 8x25fps = 200fps, so the
  // consumer must sample well above that pace or whole segments can
  // vanish between polls — a 10ms one-sample poll (observed losing
  // segments one and two entirely in a loaded local -j8 coverage run)
  // samples at most 100fps even when perfectly scheduled. Poll every
  // 1ms (~1000fps, a 5x margin over the ceiling) and keep the ~15s
  // wall window for sanitized builds that decode slower than real
  // time; the third segment ends the wait early elsewhere.
  auto* ff = static_cast<soar::FFmpegBackend*>(backend.get());
  soar::DecodedVideoFrame frame;
  bool saw_first = false;
  bool saw_second = false;
  bool saw_third = false;
  for (int i = 0; i < 15000 && !saw_third; ++i) {
    if (ff->tryGetVideoFrame(frame)) {
      saw_first = saw_first || frame.width == 160;
      saw_second = saw_second || (frame.width == 320 && frame.height == 240);
      saw_third = frame.width == 480 && frame.height == 360;
    }
    std::this_thread::sleep_for(1ms);
  }
  CHECK(saw_first);
  CHECK(saw_second);
  CHECK(saw_third);

  backend->stop();
  backend->close();
}



// Serves a directory tree with HTTP Range support: requests carrying
// "Range: bytes=a-b" get 206 + Content-Range replies, everything else a
// plain 200. python http.server answers no Range requests, and without
// Range FFmpeg reports every HTTP source as non-seekable — with it, the
// HLS VOD case below can pin real seekability and a mid-file seek.
// kRangeServerScript and httpServerReady live in test_http_servers.h,
// shared with the disk-cache tests.
using test_servers::kRangeServerScript;
using test_servers::kThrottledServerScript;

bool httpServerReady(int port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return false;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  const bool ready = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
  ::close(fd);
  return ready;
}

TEST_CASE("a stalled network source emits buffering events and recovers") {
  std::string media;
  if (!envMedia("SOAR_TEST_AUDIO_ONLY", media)) {
    MESSAGE("SOAR_TEST_AUDIO_ONLY not set; skipping buffering test");
    return;
  }
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping buffering test");
    return;
  }

  // Pipe handshake, not a TCP probe: the shared helper in
  // test_http_servers.h owns the child's lifetime (the destructor reaps
  // it even when a REQUIRE unwinds out of this case).
  auto server = test_servers::startThrottledServer(media, 15000);

  CountingSink sink;
  if (server.pid < 0) {
    MESSAGE("throttled HTTP server failed to start; skipping buffering test");
    return;
  }

  {
    auto backend = soar::makeFFmpegBackend();
    backend->setEventSink(&sink);
    REQUIRE(backend->open(soar::MediaSource{server.base_url + "/" +
                                            media.substr(media.find_last_of('/') + 1)}));
    REQUIRE(backend->play());

    // Playback drains the first burst within seconds, then reads hang for
    // the rest of the server's pause. Under heavy instrumentation the drain
    // itself can take tens of seconds, hence the generous windows; data
    // resumption ends the wait early elsewhere.
    const auto deadline = std::chrono::steady_clock::now() + 240s;
    while (std::chrono::steady_clock::now() < deadline &&
           sink.buffering_ended.load() == 0) {
      std::this_thread::sleep_for(100ms);
    }

    backend->stop();
    backend->close();
  }

  server.stop();

  CHECK(sink.buffering_started.load() >= 1);
  CHECK(sink.buffering_ended.load() >= 1);
  CHECK(sink.errors.load() == 0);
#endif
}

TEST_CASE("an A-B loop refuses to arm on a non-seekable http source") {
  std::string media;
  if (!envMedia("SOAR_TEST_AUDIO_ONLY", media)) {
    MESSAGE("SOAR_TEST_AUDIO_ONLY not set; skipping non-seekable A-B test");
    return;
  }
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping non-seekable A-B test");
    return;
  }

  // Plain 200-only server (no Range support): FFmpeg reports such sources
  // as non-seekable. A-B wrap routes through av_seek_frame, so arming on a
  // source that cannot seek must be refused up front rather than arming a
  // window the wrap machinery could never honour. The server is read-only,
  // so serving the fixture's own directory needs no scratch copy.
  const std::string name = media.substr(media.find_last_of('/') + 1);
  const std::string root = media.substr(0, media.find_last_of('/'));
  const std::string port_str = std::to_string(15400 + (::getpid() % 200));
  char* const argv[] = {
    const_cast<char*>("python3"),
    const_cast<char*>("-c"),
    const_cast<char*>(test_servers::kPlainServerScript),
    const_cast<char*>(root.c_str()),
    const_cast<char*>(port_str.c_str()),
    nullptr,
  };
  test_servers::RangeServer srv;
  srv.pid = test_servers::startPipedServer(argv);
  srv.base_url = "http://127.0.0.1:" + port_str;
  if (srv.pid < 0) {
    MESSAGE("plain HTTP server failed to start; skipping non-seekable A-B test");
    return;
  }

  {
    auto backend = soar::makeFFmpegBackend();
    REQUIRE(backend->open(soar::MediaSource{srv.base_url + "/" + name}));

    const auto info = backend->mediaInfo();
    CHECK(info.duration > 0ms);
    CHECK_FALSE(info.seekable);

    // The rejection happens before window validation: a window that would
    // be legal on a seekable twin is refused here for the source itself.
    CHECK_FALSE(backend->setLoopAB(0ms, info.duration));
    CHECK(backend->lastError().find("not seekable") != std::string::npos);
    std::chrono::milliseconds a{0}, b{0};
    CHECK_FALSE(backend->loopAB(a, b));

    backend->close();
  }

  srv.stop();
#endif
}

TEST_CASE("an HLS VOD source over a Range-capable server reports seekable and seeks") {
  // P2's user-facing promise: video-on-demand over HTTP can be dragged.
  // python http.server answers no Range requests, so every network source
  // tested so far reported non-seekable; this server speaks 206 /
  // Content-Range, the hls demuxer marks the source seekable, and a
  // seek issued while stopped resolves synchronously to the requested
  // position (the state machine contract pins that path as synchronous),
  // after which playback continues from there.
  std::string media;
  if (!envMedia("SOAR_TEST_HLS_MEDIA", media)) {
    MESSAGE("SOAR_TEST_HLS_MEDIA not set; skipping HLS seek test");
    return;
  }
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping HLS seek test");
    return;
  }
  const auto slash = media.find_last_of('/');
  const std::string dir = (slash == std::string::npos) ? std::string(".") : media.substr(0, slash);
  const std::string name = (slash == std::string::npos) ? media : media.substr(slash + 1);
  const std::string port = std::to_string(15200 + (::getpid() % 2000));

  const pid_t server = ::fork();
  REQUIRE(server >= 0);
  if (server == 0) {
    ::execlp("python3", "python3", "-c", kRangeServerScript,
             dir.c_str(), port.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }

  bool ready = false;
  for (int attempt = 0; attempt < 50 && !ready; ++attempt) {
    ready = httpServerReady(std::stoi(port));
    if (!ready) {
      std::this_thread::sleep_for(100ms);
    }
  }

  CountingSink sink;
  if (!ready) {
    MESSAGE("Range-capable HTTP server failed to start; skipping");
    ::kill(server, SIGTERM);
    ::waitpid(server, nullptr, 0);
    return;
  }

  {
    auto backend = soar::makeFFmpegBackend();
    backend->setEventSink(&sink);
    REQUIRE(backend->open(soar::MediaSource{"http://127.0.0.1:" + port + "/" + name}));
    // The 206 replies are the only reason this source is seekable; the
    // plain-http.server cases pin the opposite side of the contract.
    CHECK(backend->mediaInfo().seekable);
    CHECK(sink.errors.load() == 0);

    // Stopped-state seeks resolve synchronously: position must land on
    // the target before any playback starts.
    CHECK(backend->seek(std::chrono::milliseconds{4000}));
    CHECK(backend->position() == std::chrono::milliseconds{4000});

    // Rate up so the post-seek window resolves quickly even under heavy
    // instrumentation: playback continues from the seek target.
    REQUIRE(backend->setRate(8.0));
    REQUIRE(backend->play());
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    while (std::chrono::steady_clock::now() < deadline &&
           backend->position() < std::chrono::milliseconds{5000}) {
      std::this_thread::sleep_for(50ms);
    }
    CHECK(backend->position() >= std::chrono::milliseconds{5000});

    backend->stop();
    backend->close();
  }

  ::kill(server, SIGTERM);
  ::waitpid(server, nullptr, 0);
  CHECK(sink.errors.load() == 0);
#endif
}

// --- extraction (src/core/audio_extract.cpp) ---------------------------
// The puller half of the ASR slice: decode the best audio track and fold it
// to the 16 kHz mono s16le wire format. These cases drive it directly — it
// takes no backend, so opening a Player over the fixture would only add a
// decode thread and a video pipeline around the very thing under test.

TEST_CASE("audio extraction folds a source to 16 kHz mono s16le") {
  std::string media;
  if (!envMedia("SOAR_TEST_AUDIO_ONLY", media)) {
    MESSAGE("SOAR_TEST_AUDIO_ONLY not set; skipping extraction test");
    return;
  }
  ScratchDir dir;
  const std::string out = dir.file("tone16k.wav");

  std::string err;
  REQUIRE(soar::extractAudioToWav(media, out, &err));
  CHECK(err.empty());  // success must leave the sink alone

  const WavShape w = readWavShape(out);
  INFO(w.error);
  REQUIRE(w.ok);
  CHECK(w.audio_format == 1);  // WAVE_FORMAT_PCM
  CHECK(w.channels == 1);
  CHECK(w.sample_rate == 16000);
  CHECK(w.bits == 16);
  // The fixture is 6 s of 44.1 kHz tone; after folding to 16 kHz the payload
  // must land near 6 s. The bounds are deliberately loose: resampler delay
  // and the drain tail move the exact figure, and a slow 8 kHz-sourced variant
  // would still sit inside them.
  const double seconds = static_cast<double>(w.data_bytes) / (16000.0 * 2);
  CHECK(seconds > 5.0);
  CHECK(seconds < 6.6);
}

TEST_CASE("audio extraction takes the default track and skips other streams") {
  std::string media;
  if (!envMedia("SOAR_TEST_UNKNOWN_AUDIO_DUAL", media)) {
    MESSAGE("SOAR_TEST_UNKNOWN_AUDIO_DUAL not set; skipping dual-track test");
    return;
  }
  // h264 + aac(default) + a second audio stream whose codec id was patched to
  // an unknown one. Best-stream selection must land on the decodable AAC
  // track, and the video packets interleaved with it must be skipped rather
  // than sent to the audio decoder.
  ScratchDir dir;
  const std::string out = dir.file("dual.wav");

  std::string err;
  REQUIRE(soar::extractAudioToWav(media, out, &err));
  CHECK(err.empty());  // success must leave the sink alone

  const WavShape w = readWavShape(out);
  INFO(w.error);
  REQUIRE(w.ok);
  CHECK(w.sample_rate == 16000);
  CHECK(w.channels == 1);
  const double seconds = static_cast<double>(w.data_bytes) / (16000.0 * 2);
  CHECK(seconds > 2.5);
  CHECK(seconds < 3.6);
}

TEST_CASE("audio extraction flushes a decoder that holds its last frame") {
  std::string media;
  if (!envMedia("SOAR_TEST_WMA_AUDIO", media)) {
    MESSAGE("SOAR_TEST_WMA_AUDIO not set; skipping flush-tail test");
    return;
  }
  // wmav2 is the one decoder in the kit that still owes a frame when the
  // stream ends (probe: send all packets, then drain — wmav2 yields one
  // frame, aac/pcm/mp3/flac/opus yield none, so every other fixture runs
  // the flush loop with an empty body). The held frame must take the same
  // resample-and-write path as the streamed ones, and the duration bound
  // below is what proves it landed in the wav: a dropped tail would come
  // up one frame (~68 ms) short of the 1 s fixture.
  ScratchDir dir;
  const std::string out = dir.file("flush.wav");

  std::string err;
  REQUIRE(soar::extractAudioToWav(media, out, &err));
  CHECK(err.empty());

  const WavShape w = readWavShape(out);
  INFO(w.error);
  REQUIRE(w.ok);
  CHECK(w.sample_rate == 16000);
  CHECK(w.channels == 1);
  const double seconds = static_cast<double>(w.data_bytes) / (16000.0 * 2);
  CHECK(seconds > 0.9);
  CHECK(seconds < 1.15);
}

TEST_CASE("audio extraction upsamples an 8 kHz source to the 16 kHz wire rate") {
  std::string media;
  if (!envMedia("SOAR_TEST_MEDIA", media)) {
    MESSAGE("SOAR_TEST_MEDIA not set; skipping resample test");
    return;
  }
  // sample_dual_audio.mkv carries a 44.1 kHz and an 8 kHz audio track. Which
  // one is "best" is FFmpeg's call, so this asserts only the contract that
  // holds either way: the output is 16 kHz mono with a plausible duration.
  ScratchDir dir;
  const std::string out = dir.file("dual_audio.wav");

  std::string err;
  REQUIRE(soar::extractAudioToWav(media, out, &err));
  CHECK(err.empty());  // success must leave the sink alone

  const WavShape w = readWavShape(out);
  INFO(w.error);
  REQUIRE(w.ok);
  CHECK(w.sample_rate == 16000);
  CHECK(w.channels == 1);
  CHECK(w.data_bytes > 0);
}

TEST_CASE("audio extraction rejects a source it cannot open") {
  ScratchDir dir;
  const std::string missing = dir.file("no_such_file.mkv");
  const std::string out = dir.file("never.wav");
  std::string err;

  CHECK(!soar::extractAudioToWav(missing, out, &err));
  CHECK(err.rfind("openInput:", 0) == 0);
  // A missing source must not leave a plausible output behind.
  CHECK_FALSE(std::filesystem::exists(out));

  // The error sink is optional per the header contract.
  CHECK(!soar::extractAudioToWav(missing, out, nullptr));
}

TEST_CASE("audio extraction rejects a source with no audio track") {
  std::string media;
  if (!envMedia("SOAR_TEST_MULTI_RES", media)) {
    MESSAGE("SOAR_TEST_MULTI_RES not set; skipping no-audio-track test");
    return;
  }
  // multi_res.ts is two video streams and nothing else, so the audio lookup
  // has to fail with the explicit message instead of producing an empty WAV.
  ScratchDir dir;
  const std::string out = dir.file("video_only.wav");
  std::string err;

  CHECK(!soar::extractAudioToWav(media, out, &err));
  CHECK(err == "no audio track in source");
}

TEST_CASE("audio extraction rejects an audio codec with no decoder") {
  std::string media;
  if (!envMedia("SOAR_TEST_UNKNOWN_AUDIO", media)) {
    MESSAGE("SOAR_TEST_UNKNOWN_AUDIO not set; skipping unknown-codec test");
    return;
  }
  ScratchDir dir;
  const std::string out = dir.file("unknown.wav");
  std::string err;

  CHECK(!soar::extractAudioToWav(media, out, &err));
  CHECK(err == "no decoder for audio codec");
}

TEST_CASE("audio extraction reports an unwritable output path") {
  std::string media;
  if (!envMedia("SOAR_TEST_AUDIO_ONLY", media)) {
    MESSAGE("SOAR_TEST_AUDIO_ONLY not set; skipping output-path test");
    return;
  }
  ScratchDir dir;
  // A directory that does not exist: the muxer context can be built for the
  // "wav" format without touching the filesystem, so this fails at the file
  // open and must say so.
  const std::string out = dir.file("missing_dir/tone.wav");
  std::string err;

  CHECK(!soar::extractAudioToWav(media, out, &err));
  CHECK(err.rfind("open output:", 0) == 0);
}

TEST_CASE("audio extraction fails when the decoder refuses the packets") {
  std::string media;
  if (!envMedia("SOAR_TEST_AUDIO_REJECT", media)) {
    MESSAGE("SOAR_TEST_AUDIO_REJECT not set; skipping rejected-packet test");
    return;
  }
  // Same fixture as the playback twin above: the container opens and the
  // DTS decoder exists, but every AAC packet is refused on the sync word.
  // Whether FFmpeg surfaces that at send or at receive is version
  // behavior (the playback case says the same), so the assertion pins
  // the stage — the decode loop — rather than the exact call.
  ScratchDir dir;
  const std::string out = dir.file("reject.wav");
  std::string err;

  CHECK(!soar::extractAudioToWav(media, out, &err));
  INFO(err);
  // Stage, not call: swr_init refuses the -1 sample format when the
  // container never decoded a frame for the parameters to come from
  // (FFmpeg 8 locally), while a decoder whose format is pinned at open
  // reaches the packet loop and is refused there — the same version
  // split the playback twin documents. The container opens fine on both
  // (the twin requires it), so none of the open-stage prefixes may
  // appear; anything else in the pipeline is this fixture's refusal.
  const bool pipeline_stage =
      err.rfind("swr_alloc_set_opts2:", 0) == 0 ||
      err.rfind("swr_init:", 0) == 0 ||
      err.rfind("send_packet:", 0) == 0 ||
      err.rfind("receive_frame:", 0) == 0;
  CHECK(pipeline_stage);
}

TEST_CASE("a cached http source with unusable bytes is torn down on open failure") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping cached-open failure test");
    return;
  }
  // The disk cache arms a custom AVIO session before the demuxer probe
  // runs. When the probe fails (this file is no container), avformat_
  // open_input frees the format context but leaves a custom pb alone —
  // so the open path must free the AVIO context and drop the session
  // itself, and must not leave a half-torn-down cache for the next open.
  ScratchDir dir;
  dir.write("garbage.bin", "soar: not a media container, just bytes");
  const std::string cache_dir = dir.file("cache");
  std::error_code ec;
  std::filesystem::create_directories(cache_dir, ec);

  const test_servers::RangeServer srv =
      test_servers::startRangeServer(dir.path, 17400);
  REQUIRE(srv.pid >= 0);

  CountingSink sink;
  auto backend = soar::makeFFmpegBackend();
  backend->setEventSink(&sink);
  CHECK_FALSE(backend->open(
      soar::MediaSource{srv.base_url + "/garbage.bin", cache_dir}));
  CHECK(backend->lastError().find("failed to open") != std::string::npos);
  CHECK(sink.errors.load() >= 1);
#endif
}

#else // !SOAR_WITH_FFMPEG

// Keep the test binary meaningful when the FFmpeg backend is not compiled.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

TEST_CASE("ffmpeg backend media tests require SOAR_WITH_FFMPEG") {
  MESSAGE("FFmpeg backend not compiled in; nothing to test here");
}

#endif
