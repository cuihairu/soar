#include "cast_session.h"

#include <chrono>

namespace soar::cast {

namespace {
using Clock = std::chrono::steady_clock;
}  // namespace

CastSession::~CastSession() { stop(); }

const std::string& CastSession::lastError() const {
  std::lock_guard<std::mutex> lock(error_mu_);
  return last_error_;
}

void CastSession::restoreBridge() {
  if (exposed_.exchange(false, std::memory_order_relaxed) && bridge_ != nullptr) {
    std::string err;
    bridge_->restoreLoopback(&err);  // best effort: a failed restore is not
                                     // recoverable here, the bridge dies with
                                     // stop() anyway
  }
}

void CastSession::failWith(const std::string& message) {
  {
    std::lock_guard<std::mutex> lock(error_mu_);
    last_error_ = message;
  }
  restoreBridge();
  phase_.store(Phase::Failed, std::memory_order_release);
}

bool CastSession::start(const Params& params) {
  if (phase_.load(std::memory_order_acquire) != Phase::Idle) {
    std::lock_guard<std::mutex> lock(error_mu_);
    last_error_ = "cast: already started";
    return false;
  }
  if (params.bridge == nullptr) {
    std::lock_guard<std::mutex> lock(error_mu_);
    last_error_ = "cast: no bridge given";
    return false;
  }
  if (params.bridge->phase() != p2p::TorrentPhase::Serving) {
    std::lock_guard<std::mutex> lock(error_mu_);
    last_error_ = "cast: the bridge is not serving";
    return false;
  }
  params_ = params;
  bridge_ = params.bridge;
  stopping_ = false;
  renderer_name_.clear();
  phase_.store(Phase::Discovering, std::memory_order_release);

  std::string err;
  std::string location = params.renderer_location;
  if (location.empty()) {
    std::vector<DiscoveredRenderer> found;
    const int n = discoverRenderers(params.discover_ms, &found, &err);
    if (n < 0) {
      failWith(err);
      return false;
    }
    if (n == 0) {
      failWith("cast: no DLNA renderer answered the discovery window");
      return false;
    }
    location = found[0].location;
    renderer_name_ = found[0].friendly_name;
  }

  DeviceDesc desc;
  if (!fetchDescription(location, &desc, &err)) {
    failWith(err);
    return false;
  }
  if (renderer_name_.empty()) renderer_name_ = desc.friendly_name;

  std::string control_url;
  if (!avTransportControlUrl(desc, location, &control_url)) {
    failWith("cast: " + (desc.friendly_name.empty() ? location : desc.friendly_name) +
             " has no AVTransport service");
    return false;
  }

  UrlParts parts;
  if (!splitUrl(control_url, &parts)) {
    failWith("cast: bad control URL " + control_url);
    return false;
  }
  // Expose before SetAVTransportURI: the URL handed to the renderer has to
  // be LAN-valid when the renderer fetches it. This is the only window in
  // which the bridge listens off-host.
  if (!bridge_->exposeToLan(parts.host, &err)) {
    failWith("cast: cannot expose the bridge to the LAN: " + err);
    return false;
  }
  exposed_.store(true, std::memory_order_relaxed);

  client_ = RendererClient(control_url, kAvTransportType);
  if (!client_.setUri(bridge_->playbackUrl(), params_.title, &err)) {
    failWith("cast: SetAVTransportURI rejected: " + err);
    return false;
  }
  if (!client_.play(&err)) {
    failWith("cast: Play rejected: " + err);
    return false;
  }

  // Events are a courtesy layer over the poller: a renderer without an
  // eventSubURL, a callback URL the host cannot advertise, or a refused
  // SUBSCRIBE all leave the session poll-only, never failed.
  std::string event_url;
  if (avTransportEventUrl(desc, location, &event_url))
    startEvents(std::move(event_url), parts.host);

  poll_thread_ = std::thread([this] { pollLoop(); });
  phase_.store(Phase::Casting, std::memory_order_release);
  return true;
}

void CastSession::pollLoop() {
  std::int64_t prev_pos = -1;
  bool advanced = false;
  int consecutive_failures = 0;
  const Clock::time_point started = Clock::now();
  while (!stopping_.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(params_.poll_ms));
    if (stopping_.load(std::memory_order_relaxed)) break;
    renewEventSubscription();
    std::lock_guard<std::mutex> lock(client_mu_);
    if (stopping_.load(std::memory_order_relaxed)) break;
    PositionInfo info;
    std::string err;
    if (!client_.pollPosition(&info, &err)) {
      ++consecutive_failures;
      if (consecutive_failures >= params_.max_poll_failures) {
        failWith("cast: the renderer stopped answering: " + err);
        return;
      }
      continue;
    }
    consecutive_failures = 0;
    const std::int64_t pos = parseClockMs(info.rel_time);
    const std::int64_t dur = parseClockMs(info.duration);
    if (params_.on_position) params_.on_position(pos, dur);
    // A renderer without a parseable clock cannot be judged: no advance
    // detection, no stall failure (honest "unknown", not a false error).
    if (pos < 0) continue;
    if (prev_pos >= 0 && pos > prev_pos) advanced = true;
    if (pos > 0) advanced = true;
    // A PLAYING event over GENA is evidence of life too: some renderers
    // announce the state change before their clock starts moving.
    if (event_playing_.load(std::memory_order_acquire)) advanced = true;
    prev_pos = pos;
    // Paused time does not count against the grace window: the renderer
    // took the source (it accepted Pause), and a user pausing before the
    // clock first advances is not a decode failure.
    if (!advanced && Clock::now() - started - pausedDuration() >=
                         std::chrono::milliseconds(params_.start_grace_ms)) {
      failWith("cast: the renderer did not start playing the source — it likely "
               "cannot decode this container/codec, and this build has no "
               "sender-side transcoding");
      return;
    }
  }
}

void CastSession::stop() {
  if (phase_.load(std::memory_order_acquire) == Phase::Idle) return;
  stopping_.store(true, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(pause_mu_);
    paused_since_ = Clock::time_point{};
    paused_total_ = std::chrono::milliseconds{0};
  }
  if (poll_thread_.joinable()) poll_thread_.join();
  teardownEventSubscription();
  {
    std::lock_guard<std::mutex> lock(client_mu_);
    if (phase_.load(std::memory_order_relaxed) == Phase::Casting) {
      std::string err;
      client_.stop(&err);  // best effort: a wedged renderer still gets its
                           // bridge back; the error is not actionable here
    }
  }
  restoreBridge();
  Phase expected = Phase::Casting;
  phase_.compare_exchange_strong(expected, Phase::Stopped,
                                 std::memory_order_release,
                                 std::memory_order_relaxed);
}

bool CastSession::seekMs(std::int64_t ms) {
  if (phase_.load(std::memory_order_acquire) != Phase::Casting) return false;
  std::lock_guard<std::mutex> lock(client_mu_);
  std::string err;
  if (!client_.seekRel(ms, &err)) {
    std::lock_guard<std::mutex> elock(error_mu_);
    last_error_ = "cast: Seek rejected: " + err;
    return false;
  }
  return true;
}

void CastSession::startEvents(std::string event_url, const std::string& renderer_host) {
  std::string err;
  if (!events_.start([this](std::string_view body) { handleNotify(body); }, &err))
    return;  // no listener means no subscription; the poll path runs on
  const std::string callback = events_.callbackUrlFor(renderer_host);
  std::string response;
  Subscription sub;
  if (callback.empty() ||
      !sendEventRequest(event_url, subscribeRequest(event_url, callback, 1800),
                        &response, &err) ||
      !parseSubscribeReply(response, &sub)) {
    events_.stop();
    return;
  }
  std::lock_guard<std::mutex> lock(event_mu_);
  event_url_ = std::move(event_url);
  event_sid_ = sub.sid;
  event_expires_ =
      Clock::now() + (sub.timeout_secs == 0 ? std::chrono::hours(24)
                                            : std::chrono::seconds(sub.timeout_secs));
}

void CastSession::renewEventSubscription() {
  std::string url, sid;
  bool due = false;
  {
    std::lock_guard<std::mutex> lock(event_mu_);
    if (event_sid_.empty()) return;
    due = Clock::now() + std::chrono::seconds(60) > event_expires_;
    url = event_url_;
    sid = event_sid_;
  }
  if (!due) return;
  std::string err, response;
  Subscription sub;
  if (sendEventRequest(url, renewRequest(url, sid, 1800), &response, &err) &&
      parseSubscribeReply(response, &sub)) {
    std::lock_guard<std::mutex> lock(event_mu_);
    event_sid_ = sub.sid;
    event_expires_ =
        Clock::now() + (sub.timeout_secs == 0 ? std::chrono::hours(24)
                                              : std::chrono::seconds(sub.timeout_secs));
    return;
  }
  // The renderer dropped the subscription: poll-only from here on. A
  // late NOTIFY aimed at the dead SID is the renderer's business.
  std::lock_guard<std::mutex> lock(event_mu_);
  event_sid_.clear();
  event_url_.clear();
}

void CastSession::teardownEventSubscription() {
  std::string url, sid;
  {
    std::lock_guard<std::mutex> lock(event_mu_);
    url = std::move(event_url_);
    sid = std::move(event_sid_);
    event_url_.clear();
    event_sid_.clear();
  }
  if (!sid.empty()) {
    std::string err, response;
    sendEventRequest(url, unsubscribeRequest(url, sid), &response, &err);
  }
  events_.stop();
}

void CastSession::handleNotify(std::string_view body) {
  const TransportState state = parseNotifyState(body);
  if (state == TransportState::Playing)
    event_playing_.store(true, std::memory_order_release);
  if (state != TransportState::Unknown && params_.on_transport)
    params_.on_transport(state);
}

std::chrono::milliseconds CastSession::pausedDuration() const {
  std::lock_guard<std::mutex> lock(pause_mu_);
  std::chrono::milliseconds total = paused_total_;
  if (paused_since_ != Clock::time_point{}) {
    total += std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - paused_since_);
  }
  return total;
}

bool CastSession::paused() const {
  std::lock_guard<std::mutex> lock(pause_mu_);
  return paused_since_ != Clock::time_point{};
}

bool CastSession::pause() {
  if (phase_.load(std::memory_order_acquire) != Phase::Casting) return false;
  if (paused()) return true;  // idempotent: already holding
  std::lock_guard<std::mutex> lock(client_mu_);
  std::string err;
  if (!client_.pause(&err)) {
    std::lock_guard<std::mutex> elock(error_mu_);
    last_error_ = "cast: Pause rejected: " + err;
    return false;
  }
  std::lock_guard<std::mutex> plock(pause_mu_);
  paused_since_ = Clock::now();
  return true;
}

bool CastSession::resume() {
  if (phase_.load(std::memory_order_acquire) != Phase::Casting) return false;
  bool was_paused;
  {
    std::lock_guard<std::mutex> lock(pause_mu_);
    was_paused = paused_since_ != Clock::time_point{};
  }
  if (!was_paused) return true;  // idempotent: never paused
  std::lock_guard<std::mutex> lock(client_mu_);
  std::string err;
  if (!client_.play(&err)) {
    std::lock_guard<std::mutex> elock(error_mu_);
    last_error_ = "cast: Play rejected: " + err;
    return false;
  }
  std::lock_guard<std::mutex> plock(pause_mu_);
  paused_total_ +=
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - paused_since_);
  paused_since_ = Clock::time_point{};
  return true;
}

}  // namespace soar::cast
