#include "soar/core/backend.h"

namespace soar {
namespace {

class NullBackend final : public IBackend {
public:
  bool open(const MediaSource& source) override {
    last_ = source;
    state_ = PlaybackState::Stopped;
    return true;
  }

  bool play() override {
    state_ = PlaybackState::Playing;
    return true;
  }

  bool pause() override {
    state_ = PlaybackState::Paused;
    return true;
  }

  bool stop() override {
    state_ = PlaybackState::Stopped;
    return true;
  }

  PlaybackState state() const override {
    return state_;
  }

private:
  MediaSource last_{};
  PlaybackState state_{PlaybackState::Stopped};
};

} // namespace

std::unique_ptr<IBackend> makeNullBackend() {
  return std::make_unique<NullBackend>();
}

} // namespace soar

