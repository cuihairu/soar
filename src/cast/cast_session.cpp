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
    prev_pos = pos;
    if (!advanced && Clock::now() - started >=
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
  if (poll_thread_.joinable()) poll_thread_.join();
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

}  // namespace soar::cast
