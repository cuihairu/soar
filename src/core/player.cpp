#include "soar/core/player.h"

#include <chrono>
#include <utility>

namespace soar {

void Player::Sink::onEvent(const Event& e) {
  owner_.emit(e);
}

Player::Player(std::unique_ptr<IBackend> backend)
  : backend_(std::move(backend)), sink_(*this) {
  if (backend_) {
    backend_->setEventSink(&sink_);
  }
}

Player::~Player() {
  if (backend_) {
    backend_->setEventSink(nullptr);
  }
}

bool Player::open(MediaSource source) {
  current_ = std::move(source);
  return backend_ ? backend_->open(current_) : false;
}

void Player::close() {
  if (backend_) {
    backend_->close();
  }
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

bool Player::seek(std::chrono::milliseconds position) {
  return backend_ ? backend_->seek(position) : false;
}

bool Player::setRate(double rate) {
  return backend_ ? backend_->setRate(rate) : false;
}

bool Player::setVolume(double volume01) {
  return backend_ ? backend_->setVolume(volume01) : false;
}

bool Player::setMuted(bool muted) {
  return backend_ ? backend_->setMuted(muted) : false;
}

MediaInfo Player::mediaInfo() const {
  return backend_ ? backend_->mediaInfo() : MediaInfo{};
}

std::chrono::milliseconds Player::position() const {
  return backend_ ? backend_->position() : std::chrono::milliseconds(0);
}

bool Player::selectTrack(TrackType type, TrackId id) {
  return backend_ ? backend_->selectTrack(type, id) : false;
}

bool Player::disableSubtitles() {
  return backend_ ? backend_->disableSubtitles() : false;
}

PlaybackState Player::state() const {
  return backend_ ? backend_->state() : PlaybackState::Stopped;
}

std::string Player::lastError() const {
  return backend_ ? backend_->lastError() : std::string{};
}

void Player::setEventCallback(EventCallback cb) {
  on_event_ = std::move(cb);
}

void Player::emit(const Event& e) {
  if (on_event_) {
    on_event_(e);
  }
}

} // namespace soar
