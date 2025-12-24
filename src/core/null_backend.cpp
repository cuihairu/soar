#include "soar/core/backend.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>

namespace soar {
namespace {

class NullBackend final : public IBackend {
public:
  void setEventSink(IEventSink* sink) override {
    sink_ = sink;
  }

  bool open(const MediaSource& source) override {
    last_ = source;
    last_error_.clear();
    opened_ = true;
    state_ = PlaybackState::Stopped;
    position_ = std::chrono::milliseconds(0);
    info_ = MediaInfo{
      /*duration*/ std::chrono::minutes(10),
      /*seekable*/ true,
      /*tracks*/
      {
        TrackInfo{0, TrackType::Video, "nullvideo", "", "Video", true},
        TrackInfo{1, TrackType::Audio, "nullaudio", "und", "Audio", true},
        TrackInfo{2, TrackType::Subtitle, "nullsub", "und", "Subtitles", false},
      },
      /*selected_video*/ 0,
      /*selected_audio*/ 1,
      /*selected_subtitle*/ -1
    };
    emit(Event{EventType::MediaInfoChanged});
    emit(Event{EventType::StateChanged, state_});
    return true;
  }

  void close() override {
    opened_ = false;
    position_ = std::chrono::milliseconds(0);
    info_ = MediaInfo{};
    state_ = PlaybackState::Stopped;
    emit(Event{EventType::MediaInfoChanged});
    emit(Event{EventType::StateChanged, state_});
  }

  bool play() override {
    if (!opened_) {
      return fail("play: no media opened");
    }
    if (state_ == PlaybackState::Ended) {
      position_ = std::chrono::milliseconds(0);
      emit(Event{EventType::PositionChanged});
    }
    state_ = PlaybackState::Playing;
    emit(Event{EventType::StateChanged, state_});
    return true;
  }

  bool pause() override {
    if (!opened_) {
      return fail("pause: no media opened");
    }
    state_ = PlaybackState::Paused;
    emit(Event{EventType::StateChanged, state_});
    return true;
  }

  bool stop() override {
    if (!opened_) {
      return fail("stop: no media opened");
    }
    position_ = std::chrono::milliseconds(0);
    state_ = PlaybackState::Stopped;
    emit(Event{EventType::StateChanged, state_});
    emit(Event{EventType::PositionChanged});
    return true;
  }

  bool seek(std::chrono::milliseconds position) override {
    if (!opened_) {
      return fail("seek: no media opened");
    }
    if (!info_.seekable) {
      return fail("seek: media is not seekable");
    }

    const auto clamped = std::clamp(position, std::chrono::milliseconds(0), info_.duration);
    position_ = clamped;
    emit(Event{EventType::PositionChanged});

    if (position_ == info_.duration) {
      if (state_ != PlaybackState::Ended) {
        state_ = PlaybackState::Ended;
        emit(Event{EventType::StateChanged, state_});
      }
    } else if (state_ == PlaybackState::Ended) {
      state_ = PlaybackState::Paused;
      emit(Event{EventType::StateChanged, state_});
    }
    return true;
  }

  bool setRate(double rate) override {
    if (rate <= 0.0) {
      return fail("setRate: rate must be positive");
    }
    rate_ = rate;
    return true;
  }

  bool setVolume(double volume01) override {
    volume01_ = std::clamp(volume01, 0.0, 1.0);
    return true;
  }

  bool setMuted(bool muted) override {
    muted_ = muted;
    return true;
  }

  MediaInfo mediaInfo() const override {
    return info_;
  }

  std::chrono::milliseconds position() const override {
    return position_;
  }

  bool selectTrack(TrackType type, TrackId id) override {
    if (!opened_) {
      return fail("selectTrack: no media opened");
    }
    const auto it = std::find_if(
      info_.tracks.begin(),
      info_.tracks.end(),
      [&](const TrackInfo& t) { return t.type == type && t.id == id; }
    );
    if (it == info_.tracks.end()) {
      return fail("selectTrack: unknown track id");
    }

    if (type == TrackType::Audio) {
      info_.selected_audio = id;
      emit(Event{EventType::MediaInfoChanged});
      return true;
    }
    if (type == TrackType::Subtitle) {
      info_.selected_subtitle = id;
      emit(Event{EventType::MediaInfoChanged});
      return true;
    }
    return fail("selectTrack: video switching is not supported");
  }

  bool disableSubtitles() override {
    if (!opened_) {
      return fail("disableSubtitles: no media opened");
    }
    info_.selected_subtitle = -1;
    emit(Event{EventType::MediaInfoChanged});
    return true;
  }

  PlaybackState state() const override {
    return state_;
  }

  std::string lastError() const override {
    return last_error_;
  }

private:
  bool fail(std::string message) {
    last_error_ = std::move(message);
    emit(Event{EventType::Error, state_, position_, last_error_});
    return false;
  }

  void emit(Event e) {
    if (!sink_) {
      return;
    }
    e.state = state_;
    e.position = position_;
    sink_->onEvent(e);
  }

  IEventSink* sink_{nullptr};
  MediaSource last_{};
  MediaInfo info_{};
  std::chrono::milliseconds position_{0};
  PlaybackState state_{PlaybackState::Stopped};
  std::string last_error_{};
  bool opened_{false};
  double rate_{1.0};
  double volume01_{1.0};
  bool muted_{false};
};

} // namespace

std::unique_ptr<IBackend> makeNullBackend() {
  return std::make_unique<NullBackend>();
}

} // namespace soar
