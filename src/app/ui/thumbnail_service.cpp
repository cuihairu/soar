#include "ui/thumbnail_service.h"

namespace soar::app {

ThumbnailService::ThumbnailService(Grab grab, std::size_t cache_cap)
    : grab_(std::move(grab)), cache_cap_(cache_cap) {
  worker_ = std::thread([this] { run(); });
}

ThumbnailService::~ThumbnailService() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  worker_.join();
}

void ThumbnailService::request(std::chrono::milliseconds position) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (pending_ && *pending_ == position) return;  // already queued
  if (cache_.count(position) != 0) return;        // already decoded
  pending_ = position;
  cv_.notify_one();
}

bool ThumbnailService::tryGet(std::chrono::milliseconds position,
                              ThumbnailFrame& out) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = cache_.find(position);
  if (it == cache_.end()) return false;
  // LRU touch: the just-read entry is the most recently used.
  order_.remove(position);
  order_.push_back(position);
  out = it->second;
  return true;
}

void ThumbnailService::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  pending_.reset();
  cache_.clear();
  order_.clear();
}

void ThumbnailService::run() {
  for (;;) {
    std::chrono::milliseconds job{0};
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [&] { return stop_ || pending_.has_value(); });
      if (stop_) return;
      job = *pending_;
      pending_.reset();
    }
    ThumbnailFrame frame;
    if (grab_(job, frame)) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (cache_.count(job) == 0) {
        if (cache_.size() >= cache_cap_ && !order_.empty()) {
          // Evict the least recently used entry (never the one just
          // produced, which is appended below).
          cache_.erase(order_.front());
          order_.pop_front();
        }
        cache_[job] = std::move(frame);
        order_.push_back(job);
      }
    }
  }
}

}  // namespace soar::app
