// The wire half of the casting control point: one M-SEARCH burst over
// UDP multicast, plain-HTTP fetches of device descriptions, SOAP action
// POSTs, and the local-address lookup the bridge's LAN exposure needs.
//
// Same self-contained socket layer as src/p2p (winsock on Windows, POSIX
// elsewhere). Every call reports failure through its `err` out-param and
// returns false; nothing throws, nothing blocks past its timeout.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "device_xml.h"
#include "soap.h"

namespace soar::cast {

// One renderer heard on the discovery window.
struct DiscoveredRenderer {
  std::string location;       // device description URL (LOCATION)
  std::string friendly_name;  // from the description; "" if it never loaded
  std::string usn;            // unique service name (the dedup key)
};

// Sends one M-SEARCH burst for AVTransport renderers and collects unicast
// replies for `timeout_ms`. Deduplicates by USN and best-effort loads each
// device description for the friendly name (a description that fails to
// load leaves the name empty but the entry usable). `multicast_if` selects
// the outgoing interface as a dotted quad — nullptr means the system's
// multicast route (product), "127.0.0.1" keeps the exchange on loopback
// (the in-process renderer tests). Returns the number of unique renderers.
int discoverRenderers(int timeout_ms, std::vector<DiscoveredRenderer>* out,
                      std::string* err, const char* multicast_if = nullptr);

// HTTP GET of a device description, parsed. `location` is the URL the
// M-SEARCH reply carried.
bool fetchDescription(const std::string& location, DeviceDesc* out, std::string* err);

// A client bound to one renderer's AVTransport control URL. Methods map
// one-to-one onto the five transport actions of this batch plus the state
// read; every method opens its own short-lived connection, so a renderer
// that wedges one request cannot poison the next.
class RendererClient {
 public:
  RendererClient() = default;
  RendererClient(std::string control_url, std::string service_type)
      : control_url_(std::move(control_url)), service_type_(std::move(service_type)) {}

  // All five return false and fill `err` on transport failure (connect,
  // write, read, timeout) or when the renderer answers with a SOAP fault
  // (the fault text rides along: "SOAP fault 716: Resource not found").
  bool setUri(const std::string& uri, const std::string& title, std::string* err);
  bool play(std::string* err);
  bool pause(std::string* err);
  bool stop(std::string* err);
  bool seekRel(std::int64_t ms, std::string* err);

  // GetPositionInfo. Unlike the actions, a fault here is reported as a
  // failure too, but an unparseable (non-position) body fails without a
  // fault note — the poller treats repeated failures on their own.
  bool pollPosition(PositionInfo* out, std::string* err);

 private:
  // Sends a prebuilt request, returns the body and status of the reply.
  bool post(const std::string& request, int* status, std::string* body, std::string* err);

  std::string control_url_;
  std::string service_type_;
};

// The GENA subscription family runs over the same plumbing: one request
// (a builder from eventing.h), the raw reply text back — the reply head
// matters here, since SID and TIMEOUT are headers, not body content.
// False on a transport failure; the reply's status is parsed downstream.
bool sendEventRequest(const std::string& event_url, const std::string& request,
                      std::string* response, std::string* err);

}  // namespace soar::cast
