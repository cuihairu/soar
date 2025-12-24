#pragma once

#include "soar/core/backend.h"

#include <chrono>
#include <functional>
#include <memory>

namespace soar {

class Player {
public:
  using EventCallback = std::function<void(const Event&)>;

public:
  explicit Player(std::unique_ptr<IBackend> backend);
  ~Player();

  Player(const Player&) = delete;
  Player& operator=(const Player&) = delete;
  Player(Player&&) = delete;
  Player& operator=(Player&&) = delete;

  bool open(MediaSource source);
  void close();
  bool play();
  bool pause();
  bool stop();

  bool seek(std::chrono::milliseconds position);
  bool setRate(double rate);
  bool setVolume(double volume01);
  bool setMuted(bool muted);

  MediaInfo mediaInfo() const;
  std::chrono::milliseconds position() const;

  bool selectTrack(TrackType type, TrackId id);
  bool disableSubtitles();

  PlaybackState state() const;
  std::string lastError() const;

  void setEventCallback(EventCallback cb);

private:
  class Sink final : public IEventSink {
  public:
    explicit Sink(Player& owner) : owner_(owner) {}
    void onEvent(const Event& e) override;

  private:
    Player& owner_;
  };

  void emit(const Event& e);

  std::unique_ptr<IBackend> backend_;
  MediaSource current_;
  EventCallback on_event_{};
  Sink sink_;
};

std::unique_ptr<IBackend> makeNullBackend();

} // namespace soar
