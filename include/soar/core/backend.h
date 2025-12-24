#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace soar {

struct MediaSource {
  std::string uri;
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
  Error
};

struct Event {
  EventType type{EventType::StateChanged};
  PlaybackState state{PlaybackState::Stopped};
  std::chrono::milliseconds position{0};
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
