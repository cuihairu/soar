// The NOTIFY endpoint a renderer pushes GENA events to: a TCP listener
// that lives exactly as long as the casting session (the same exposure
// window the bridge's LAN binding has), answers every request 200, and
// hands each body to the session's parser. One self-contained socket
// layer, same as renderer.cpp — nothing is shared across cast TUs.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <thread>

namespace soar::cast {

class EventServer {
 public:
  EventServer() = default;
  ~EventServer();
  EventServer(const EventServer&) = delete;
  EventServer& operator=(const EventServer&) = delete;

  // Binds 0.0.0.0 on an ephemeral port and serves until stop(). Every
  // received body goes to `on_notify` (called on the serve thread — keep
  // it lock-safe, not long-running). False when the bind fails, with the
  // reason in `err`.
  bool start(std::function<void(std::string_view)> on_notify, std::string* err);
  void stop();

  // The URL a renderer at `peer_host` can reach this server on: the local
  // interface that routes toward that peer (the UDP-connect probe the
  // bridge's LAN exposure uses), with the fixed callback path. "" when
  // nothing routes or the server is not up.
  std::string callbackUrlFor(const std::string& peer_host) const;

  int port() const { return port_; }

 private:
  void serveLoop();

  std::atomic<bool> running_{false};
  int listen_fd_ = -1;
  int port_ = 0;
  std::function<void(std::string_view)> on_notify_;
  std::thread serve_thread_;
};

}  // namespace soar::cast
