// One casting session (docs/mvp.md §3, DLNA control point): renderer
// discovery (or an explicit device-description URL), the AVTransport
// exchange — SetAVTransportURI, Play, Seek, Stop — position polling via
// GetPositionInfo (GENA eventing is deliberately not done), and the
// bridge's LAN-exposure window around it all.
//
// The local bridge stays 127.0.0.1-only outside a session; start() exposes
// it to the LAN for exactly as long as the renderer may fetch from it and
// stop() (or any failure path) puts the loopback binding back. The
// receiver decodes the source itself: a renderer that never starts
// playing is reported as an explicit decode failure — this build has no
// sender-side transcoding to fall back on.
//
// Thread contract: start()/stop()/seekMs()/phase() all come from the
// owner thread (the one that called start()); a poll thread inside reads
// nothing the owner mutates except through the mutex-guarded client.
// stop() waits at most one poll round for the poller to notice the flag.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "renderer.h"
#include "torrent_stream.h"

namespace soar::cast {

class CastSession {
 public:
  struct Params {
    // The serving torrent bridge whose URL the renderer fetches. Required.
    p2p::TorrentStream* bridge = nullptr;
    // Explicit device-description URL: skips discovery entirely. Empty =
    // one M-SEARCH burst for `discover_ms` and the first reply wins.
    std::string renderer_location;
    int discover_ms = 3000;
    std::string title;  // dc:title of the DIDL metadata ("")
    // ~1 Hz tick with the renderer-reported position/duration in ms (-1 =
    // the renderer did not report a parseable clock for that field).
    std::function<void(std::int64_t, std::int64_t)> on_position;
    // Poll cadence, the no-advance window after Play (a receiver that
    // cannot decode the source sits at 0:00 — that is the explicit
    // failure), and how many consecutive failed polls end the session.
    int poll_ms = 1000;
    int start_grace_ms = 15000;
    int max_poll_failures = 10;
  };

  // Idle → Discovering → Casting → Stopped on a clean end; any failure
  // publishes Failed with lastError() (and restores the bridge) either
  // synchronously from start() or from the poll thread.
  enum class Phase { Idle, Discovering, Casting, Failed, Stopped };

  CastSession() = default;
  ~CastSession();
  CastSession(const CastSession&) = delete;
  CastSession& operator=(const CastSession&) = delete;

  // Discovery → bridge LAN exposure → SetAVTransportURI → Play → poll
  // thread. Returns false with lastError() set (the bridge is back on
  // loopback in every failure path).
  bool start(const Params& params);

  // SOAP Stop (best effort), poll-thread join, bridge restore. Idempotent;
  // also the destructor's cleanup.
  void stop();

  // REL_TIME seek at the renderer. False when not casting or rejected
  // (the reason lands in lastError()).
  bool seekMs(std::int64_t ms);

  Phase phase() const { return phase_.load(std::memory_order_acquire); }
  const std::string& lastError() const;
  const std::string& rendererName() const { return renderer_name_; }

 private:
  void pollLoop();
  // Records `message` as lastError(), restores the bridge, publishes
  // Failed. Safe from both the owner and the poll thread.
  void failWith(const std::string& message);
  void restoreBridge();

  std::atomic<Phase> phase_{Phase::Idle};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> exposed_{false};
  mutable std::mutex error_mu_;  // guards last_error_
  std::string last_error_;
  std::string renderer_name_;
  Params params_;
  p2p::TorrentStream* bridge_ = nullptr;
  std::thread poll_thread_;
  std::mutex client_mu_;  // guards client_ between the poll thread and seekMs/stop
  RendererClient client_;
};

}  // namespace soar::cast
