#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace soar {

struct MediaSource {
  std::string uri;
  // When non-empty and the source is an http:// url, the FFmpeg backend
  // caches downloaded data under this directory so the same url can be
  // replayed offline (and P3b can resume partial downloads). Ignored for
  // local paths and https:// sources.
  std::string cache_dir;
};

using TrackId = std::int32_t;

enum class TrackType {
  Video,
  Audio,
  Subtitle
};

struct TrackInfo {
  TrackId id{};
  TrackType type{TrackType::Video};
  std::string codec;
  std::string language;
  std::string title;
  bool is_default{false};
};

struct MediaInfo {
  std::chrono::milliseconds duration{0};
  bool seekable{false};
  std::vector<TrackInfo> tracks;
  TrackId selected_video{-1};
  TrackId selected_audio{-1};
  TrackId selected_subtitle{-1};
};

enum class PlaybackState {
  Stopped,
  Paused,
  Playing,
  Ended,
  Error
};

enum class EventType {
  StateChanged,
  MediaInfoChanged,
  PositionChanged,
  Error,
  // Network-source progress: the backend stopped receiving data fast
  // enough to keep playing (BufferingStarted) and resumed doing so
  // (BufferingEnded). Local media never emits these.
  BufferingStarted,
  BufferingEnded,
  // P3c download progress: while playing through the http:// disk cache,
  // emitted as cached bytes grow — downloaded counts cached bytes out of
  // total source bytes. Throttled to 1/16 steps of the source size, so the
  // series is bounded and monotonic with a terminal downloaded == total
  // event. Non-cache sources (local files, https pass-through) never
  // emit it.
  DownloadProgress
};

struct Event {
  EventType type{EventType::StateChanged};
  PlaybackState state{PlaybackState::Stopped};
  std::chrono::milliseconds position{0};
  // Human-readable payload. Error carries the message; DownloadProgress
  // carries "downloaded/total" in decimal bytes (e.g. "524288/531609").
  // Do not add fields to this struct: every Event{...} construction site
  // pays GCCounter exception-cleanup branches that scale with the member
  // count, and those branches are structurally un-executable (never
  // executed), which permanently drags the branch-coverage gate down
  // (docs/coverage-notes.md §3.8). Pack extra payloads into `message`
  // with a documented format instead.
  std::string message;
};

class IEventSink {
public:
  virtual ~IEventSink() = default;
  virtual void onEvent(const Event& e) = 0;
};

class IBackend {
public:
  virtual ~IBackend() = default;

  virtual void setEventSink(IEventSink* sink) = 0;

  virtual bool open(const MediaSource& source) = 0;
  virtual void close() = 0;

  virtual bool play() = 0;
  virtual bool pause() = 0;
  virtual bool stop() = 0;

  virtual bool seek(std::chrono::milliseconds position) = 0;
  virtual bool setRate(double rate) = 0;
  virtual bool setVolume(double volume01) = 0;
  virtual bool setMuted(bool muted) = 0;

  virtual MediaInfo mediaInfo() const = 0;
  virtual std::chrono::milliseconds position() const = 0;

  virtual bool selectTrack(TrackType type, TrackId id) = 0;
  virtual bool disableSubtitles() = 0;

  virtual PlaybackState state() const = 0;
  virtual std::string lastError() const = 0;
};

} // namespace soar
