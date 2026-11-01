// Seek-bar hover thumbnail worker (docs/ui-design.md §1.6-5, the v1 seek
// preview): one side thread decodes frames for hovered seek positions while
// the UI thread only requests and polls — the reopen-per-call grab
// (exportSubtitleText's contract) never blocks the UI. Pure logic over
// plain bytes: no FFmpeg, SDL or ImGui headers here on purpose (the same
// contract as ui_state.h/library.h), so the window layer stays the only
// place that knows what a decoder is.

#ifndef SOAR_APP_UI_THUMBNAIL_SERVICE_H_
#define SOAR_APP_UI_THUMBNAIL_SERVICE_H_

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace soar::app {

// One decoded thumbnail. Plain bytes + geometry — the service never sees a
// decoder type; the grab callback fills it (tightly packed YUV420P, the
// same plane layout the video mailbox hands the UI).
struct ThumbnailFrame {
  int width{0};
  int height{0};
  int stride_y{0};
  int stride_u{0};
  int stride_v{0};
  std::vector<std::uint8_t> y;
  std::vector<std::uint8_t> u;
  std::vector<std::uint8_t> v;
};

// One worker + a small LRU. A newer request supersedes the queued one (the
// pointer keeps moving), cache hits answer without a decode (scrubbing
// re-asks the same offsets), and destruction joins the worker — the last
// in-flight grab may still cost its own decode time, which the hover
// feature accepts (a local file decodes in milliseconds; the network
// reopen boundary is the backend's, and documented there).
class ThumbnailService {
 public:
  // (position, out) -> decoded. Called on the worker thread only; must be
  // safe to run while playback continues (the backend contract above).
  using Grab = std::function<bool(std::chrono::milliseconds, ThumbnailFrame&)>;

  explicit ThumbnailService(Grab grab, std::size_t cache_cap = 24);
  ~ThumbnailService();
  ThumbnailService(const ThumbnailService&) = delete;
  ThumbnailService& operator=(const ThumbnailService&) = delete;

  // UI thread: ask for `position`. A repeated identical request is a no-op,
  // a cached position is not re-decoded, and any queued-but-unstarted
  // request is replaced (only the newest hover matters).
  void request(std::chrono::milliseconds position);

  // UI thread: poll. True with `out` filled when the position is ready —
  // a cache hit or a finished decode. False while the decode is still
  // running (the UI keeps the placeholder and asks again next frame).
  bool tryGet(std::chrono::milliseconds position, ThumbnailFrame& out);

  // UI thread: drop every cached/queued frame. Called when the media
  // source changes — positions are meaningless across files, so a stale
  // thumbnail must never survive a source switch. Safe to call from the
  // UI thread; the worker joins the reset at its next queue poll.
  void clear();

 private:
  void run();

  Grab grab_;
  std::size_t cache_cap_;
  std::thread worker_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::optional<std::chrono::milliseconds> pending_;
  std::map<std::chrono::milliseconds, ThumbnailFrame> cache_;
  std::list<std::chrono::milliseconds> order_;  // LRU order, front = oldest
};

}  // namespace soar::app

#endif  // SOAR_APP_UI_THUMBNAIL_SERVICE_H_
