#pragma once

#include "soar/core/backend.h"

#include <memory>

namespace soar {

class Player {
public:
  explicit Player(std::unique_ptr<IBackend> backend);

  bool open(MediaSource source);
  bool play();
  bool pause();
  bool stop();

  PlaybackState state() const;

private:
  std::unique_ptr<IBackend> backend_;
  MediaSource current_;
};

std::unique_ptr<IBackend> makeNullBackend();

} // namespace soar

