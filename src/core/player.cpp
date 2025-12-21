#include "soar/core/player.h"

#include <utility>

namespace soar {

Player::Player(std::unique_ptr<IBackend> backend) : backend_(std::move(backend)) {}

bool Player::open(MediaSource source) {
  current_ = std::move(source);
  return backend_ ? backend_->open(current_) : false;
}

bool Player::play() {
  return backend_ ? backend_->play() : false;
}

bool Player::pause() {
  return backend_ ? backend_->pause() : false;
}

bool Player::stop() {
  return backend_ ? backend_->stop() : false;
}

PlaybackState Player::state() const {
  return backend_ ? backend_->state() : PlaybackState::Stopped;
}

} // namespace soar

