// GENA eventing for the casting control point (docs/mvp.md §3): the
// SUBSCRIBE family for the AVTransport event stream and the parsing of
// the two payloads that matter — a subscription reply (SID + timeout)
// and a NOTIFY body's LastChange report (the transport state). Pure
// string work; the wire sides are renderer.cpp (outbound requests) and
// event_server.cpp (the callback endpoint that receives NOTIFY).
#pragma once

#include <string>
#include <string_view>

namespace soar::cast {

// The transport states a NOTIFY can report — the val= spellings of the
// AVTransport spec's TransportState. Unknown covers anything unparsable,
// including a body with no state report at all.
enum class TransportState {
  Unknown,
  Playing,
  PausedPlayback,
  Stopped,
  Transitioning,
  NoMediaPresent,
};

// An accepted subscription.
struct Subscription {
  std::string sid;       // the renderer-assigned "uuid:..." id
  int timeout_secs = 0;  // what the renderer actually granted (0 = infinite)
};

// Initial subscription: CALLBACK names the control point's event endpoint
// in its angle-bracket wire form, NT names the event class. The timeout
// is clamped to the 300..86400 range the spec's Second- field implies.
std::string subscribeRequest(const std::string& event_url,
                             const std::string& callback_url, int timeout_secs);

// Renewal and teardown both address an existing SID (and carry neither
// CALLBACK nor NT; the spec forbids them there).
std::string renewRequest(const std::string& event_url, const std::string& sid,
                         int timeout_secs);
std::string unsubscribeRequest(const std::string& event_url, const std::string& sid);

// SID and TIMEOUT of a 200 reply to SUBSCRIBE. False when the status is
// not 200 or either field is missing — an error reply is not a
// subscription, and "infinite" grants parse to timeout 0 (no renewal).
bool parseSubscribeReply(std::string_view response, Subscription* out);

// The transport state of a NOTIFY body. The state rides the escaped
// <LastChange val="..."> attribute: one xmlDecode turns that into the
// embedded <Event> document, then <TransportState val="..."> is read out
// of it. Unknown when either step finds nothing.
TransportState parseNotifyState(std::string_view notify_body);

}  // namespace soar::cast
