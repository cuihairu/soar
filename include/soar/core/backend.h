#pragma once

#include <string>

namespace soar {

struct MediaSource {
  std::string uri;
};

enum class PlaybackState {
  Stopped,
  Paused,
  Playing
};

class IBackend {
public:
  virtual ~IBackend() = default;

  virtual bool open(const MediaSource& source) = 0;
  virtual bool play() = 0;
  virtual bool pause() = 0;
  virtual bool stop() = 0;
  virtual PlaybackState state() const = 0;
};

} // namespace soar

