// Sender-side transcoding for casts (docs/mvp.md §3): the source the
// bridge serves is not always something a DLNA renderer can decode (HEVC,
// VP9, DTS/AC-3 audio in MKV containers are common). When the session
// asks for transcode support, this server demuxes the bridge URL, decodes
// the media streams, re-encodes to the MPEG-TS-safe pair (H.264 video,
// AAC audio; streams that already are compatible are copied through
// untouched), muxes to MPEG-TS, and serves the result over a self-
// contained HTTP endpoint the renderer fetches like any other URI.
//
// Without the FFmpeg backend (SOAR_WITH_FFMPEG undefined) the class
// compiles to its inert stub branch: start() fails with the explicit
// reason and the session either casts the direct URL or fails honestly —
// there is no half-working transcode path.
//
// The endpoint is a live point: bytes flow from the transcode pump to
// whoever is connected, there is no stored copy, and HTTP range requests
// have nothing to seek into — a session running on this server reports
// Seek as unsupported. The pump is backpressured by the queue the HTTP
// side drains: a slow or stalled renderer slows the transcode, which
// slows the bridge reads, which is exactly the right flow control.
//
// Thread contract: start()/stop()/waitReady()/urlFor() come from the
// session thread; the pump and the HTTP accept/serve threads live
// inside. stop() is idempotent and joins both.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace soar::cast {

class TranscodeServer {
 public:
  TranscodeServer() = default;
  ~TranscodeServer();
  TranscodeServer(const TranscodeServer&) = delete;
  TranscodeServer& operator=(const TranscodeServer&) = delete;

  // Opens `source_url` with the demuxer, starts the pump and the HTTP
  // listener (0.0.0.0, ephemeral port). False when the listener cannot
  // bind or the build has no FFmpeg backend, with the reason in *err;
  // pump-side problems (bad source, no decodable streams) surface through
  // waitReady() instead so callers get one readiness answer.
  bool start(const std::string& source_url, std::string* err);
  void stop();

  // Blocks until the first MPEG-TS bytes are queued (the muxer header is
  // out, a renderer connecting now sees a stream at once), the pump
  // failed, or the timeout passed. False on failure — lastError() says
  // which.
  bool waitReady(int timeout_ms);

  // The URL a renderer at `peer_host` reaches the endpoint on (the same
  // UDP-connect probe the bridge exposure and the event callback use).
  // "" when the server is not up or nothing routes.
  std::string urlFor(const std::string& peer_host) const;

  int port() const { return port_; }
  const std::string& lastError() const;

  // The pump's failure channel (it runs on a member thread but reports
  // through the same mutex-guarded message lastError() reads).
  void recordError(const std::string& message);

  // The producer side of the queue, called from the muxer's AVIO write
  // callback (the .cpp-local trampoline lands here). The cap is the
  // backpressure: a slow renderer slows the whole transcode chain down
  // to the bridge read.
  bool pushBytes(const unsigned char* data, int size);

 private:
  // The pump: demux → decode/encode → mux, pushed through the queue.
  // Real body under the FFmpeg backend; without it never called.
  void pumpLoop();
  // Accept loop: reads the one request head per connection, hands the
  // connection to the stream thread (a second concurrent fetch is told
  // 503 — one renderer at a time). Unreadable heads (silence, reset) are
  // dropped without a reply; any head that completes is served the one
  // resource.
  // `listen_fd` is the thread's own copy (stop() closes the member to
  // wake the loop — the accept on Darwin, where shutdown on a listener
  // is a no-op — and must not race the loop's reads).
  void acceptLoop(int listen_fd);
  // Stream thread body: the 200 head, then queue bytes until the client
  // disconnects, the pump ends (eof) or stop() tears it down.
  void serveStream(int fd);
  // Pops up to `max` bytes; false when stopping or the pump hit EOF/error
  // after draining. `*eof` says which drain-end it was.
  bool popBytes(std::vector<char>* out, size_t max, bool* eof);

  std::string source_url_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> stream_active_{false};
  // Guards stream_thread_ between the accept loop (reap before respawn)
  // and stop() (final join) — two threads joining the same std::thread is
  // a race, so both go through this.
  std::mutex stream_mu_;
  int listen_fd_ = -1;
  int port_ = 0;
  std::thread pump_thread_;
  std::thread accept_thread_;
  std::thread stream_thread_;

  // The byte queue between pump and HTTP serve. bytes_mu_ guards all of
  // it; bytes_cv_ wakes pushers (queue drained / stopping) and poppers
  // (data / EOF / stopping / first bytes for waitReady).
  mutable std::mutex bytes_mu_;
  std::condition_variable bytes_cv_;
  std::deque<std::vector<char>> queue_;
  size_t queued_bytes_ = 0;
  bool first_bytes_ = false;   // muxer header seen: waitReady's green light
  bool eof_ = false;           // pump finished (clean or after an error)
  mutable std::mutex error_mu_;
  std::string last_error_;
};

}  // namespace soar::cast
