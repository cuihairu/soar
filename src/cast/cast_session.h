// One casting session (docs/mvp.md §3, DLNA control point): renderer
// discovery (or an explicit device-description URL), the AVTransport
// exchange — SetAVTransportURI, Play, Pause, Seek, Stop — position
// polling via GetPositionInfo with GENA eventing alongside it (a NOTIFY
// that reports PLAYING counts as evidence of life for the start-grace
// window; the poller stays the primary state source), and the bridge's
// LAN-exposure window around it all.
//
// The local bridge stays 127.0.0.1-only outside a session; start() exposes
// it to the LAN for exactly as long as the renderer may fetch from it and
// stop() (or any failure path) puts the loopback binding back. The
// receiver decodes the source itself: a renderer that never starts
// playing is reported as an explicit decode failure. With transcode
// support (SOAR_WITH_FFMPEG and Params::transcode) the session instead
// re-encodes to H.264/AAC MPEG-TS and points the renderer at that — a
// renderer stalling even on the transcoded stream is the honest failure.
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

#include "event_server.h"
#include "eventing.h"
#include "renderer.h"
#include "torrent_stream.h"
#include "transcode_server.h"

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
    // Sender-side transcoding (needs the FFmpeg backend): demux the bridge
    // source, re-encode to H.264/AAC MPEG-TS, serve it over the session's
    // own HTTP endpoint and hand the renderer that URI instead. A build
    // without FFmpeg fails the session with the explicit reason.
    bool transcode = false;
    // ~1 Hz tick with the renderer-reported position/duration in ms (-1 =
    // the renderer did not report a parseable clock for that field).
    std::function<void(std::int64_t, std::int64_t)> on_position;
    // GENA events (best-effort): the transport state a NOTIFY reports,
    // delivered on the event-server thread. May be unset; unparsable
    // bodies are not reported, and a session without a working
    // subscription simply stays poll-only.
    std::function<void(TransportState)> on_transport;
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

  // Discovery → bridge LAN exposure → (transcode start) → SetAVTransportURI
  // → Play → poll thread. Returns false with lastError() set (the bridge is
  // back on loopback in every failure path).
  bool start(const Params& params);

  // SOAP Stop (best effort), transcode teardown, poll-thread join, bridge
  // restore. Idempotent; also the destructor's cleanup.
  void stop();

  // REL_TIME seek at the renderer. False when not casting or rejected
  // (the reason lands in lastError()). Always false while transcoding:
  // the served MPEG-TS is a live point with nothing to seek into.
  bool seekMs(std::int64_t ms);

  // AVTransport Pause. Idempotent. Paused time is excluded from the
  // start-grace window: the renderer demonstrably took the source when it
  // accepted Pause, and a user pausing before the clock first advances is
  // not a decode failure.
  bool pause();
  // Resume is Play at speed 1 — the spec has no separate Resume action.
  bool resume();

  // Whether the session believes the renderer is currently paused. The
  // renderer's own view is polled position, which freezes while paused.
  bool paused() const;

  Phase phase() const { return phase_.load(std::memory_order_acquire); }
  const std::string& lastError() const;
  const std::string& rendererName() const { return renderer_name_; }

 private:
  void pollLoop();
  // Records `message` as lastError(), restores the bridge, publishes
  // Failed. Safe from both the owner and the poll thread.
  void failWith(const std::string& message);
  void restoreBridge();

  // GENA eventing (best-effort): a subscription that never lands leaves
  // the session poll-only. event_mu_ guards the subscription record
  // between the owner thread (subscribe, teardown) and the poll thread
  // (renewal once fewer than 60 seconds remain).
  void startEvents(std::string event_url, const std::string& renderer_host);
  void renewEventSubscription();
  void teardownEventSubscription();
  void handleNotify(std::string_view body);

  EventServer events_;
  mutable std::mutex event_mu_;
  std::string event_url_;
  std::string event_sid_;
  std::chrono::steady_clock::time_point event_expires_{};
  std::atomic<bool> event_playing_{false};  // the latest NOTIFY said PLAYING

  // Paused-wall-clock bookkeeping (pause_mu_ guards both members; the poll
  // thread calls it through pausedDuration()).
  mutable std::mutex pause_mu_;
  std::chrono::steady_clock::time_point paused_since_{};  // epoch = up
  std::chrono::milliseconds paused_total_{0};
  std::chrono::milliseconds pausedDuration() const;

  std::atomic<Phase> phase_{Phase::Idle};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> exposed_{false};
  mutable std::mutex error_mu_;  // guards last_error_
  std::string last_error_;
  std::string renderer_name_;
  Params params_;
  p2p::TorrentStream* bridge_ = nullptr;
  TranscodeServer transcode_;
  std::thread poll_thread_;
  std::mutex client_mu_;  // guards client_ between the poll thread and seekMs/stop
  RendererClient client_;
};

}  // namespace soar::cast
