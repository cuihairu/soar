// Tests for the DLNA casting control point (src/cast, docs/mvp.md §3,
// protocol decided 2026-10-11). The pure string logic — SSDP messages,
// device-description parsing, SOAP envelopes, URL handling — runs on
// every platform. The socket half runs against a minimal in-process
// MediaRenderer (same shape as the embedded RTSP server of the media
// suite): discovery exchange on loopback multicast, description fetch,
// the action loop, position polling and the bridge LAN rebind end to
// end. POSIX only for those, same gate as the p2p suite.

// Custom main, only to line-buffer stdout: under ctest the capture pipe
// flips libc to full buffering, and a hung suite then leaves no trace in
// the captured log (the p2p suite's 2026-10-07 lesson).
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "cast_session.h"
#include "device_xml.h"
#include "soap.h"
#include "ssdp.h"
#include "text_util.h"
#include "torrent_stream.h"
#include "transcode_server.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <libtorrent/bencode.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/file_storage.hpp>

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IOLBF, 1024);
  return doctest::Context(argc, argv).run();
}

// Case-start breadcrumb, same as the p2p suite: a hung run names its own
// case in the captured ctest output.
namespace {
struct CaseStartListener : doctest::IReporter {
  explicit CaseStartListener(const doctest::ContextOptions&) {}
  void report_query(const doctest::QueryData&) override {}
  void test_run_start() override {}
  void test_run_end(const doctest::TestRunStats&) override {}
  void test_case_start(const doctest::TestCaseData& in) override {
    std::printf("[case] %s:%u %s\n", in.m_file.c_str(), in.m_line, in.m_name);
  }
  void test_case_reenter(const doctest::TestCaseData&) override {}
  void test_case_end(const doctest::CurrentTestCaseStats&) override {}
  void test_case_exception(const doctest::TestCaseException&) override {}
  void subcase_start(const doctest::SubcaseSignature&) override {}
  void subcase_end() override {}
  void log_assert(const doctest::AssertData&) override {}
  void log_message(const doctest::MessageData&) override {}
  void test_case_skipped(const doctest::TestCaseData&) override {}
};
DOCTEST_REGISTER_LISTENER("case_start_cast", 0, CaseStartListener);
}  // namespace

namespace lt = libtorrent;
namespace cast = soar::cast;
namespace p2p = soar::p2p;

// ---------------------------------------------------------------------------
// Pure string logic — every platform.
// ---------------------------------------------------------------------------

TEST_CASE("M-SEARCH requests carry the discovery target and a clamped MX") {
  const std::string req = cast::makeSearchRequest(cast::kAvTransportType, 9);
  CHECK(req ==
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 5\r\n"
        "ST: urn:schemas-upnp-org:service:AVTransport:1\r\n"
        "\r\n");
  CHECK(cast::makeSearchRequest("st", 0).find("MX: 1\r\n") != std::string::npos);
  CHECK(cast::makeSearchRequest("st", 3).find("MX: 3\r\n") != std::string::npos);
}

TEST_CASE("search replies parse and announcements are refused") {
  cast::SearchResponse r;
  CHECK(cast::parseSearchResponse(
      "HTTP/1.1 200 OK\r\n"
      "CACHE-CONTROL: max-age=1800\r\n"
      "LOCATION: http://192.168.1.20:8200/desc\r\n"
      "SERVER: Linux/1.0 UPnP/1.0 renderer/1.0\r\n"
      "ST: urn:schemas-upnp-org:service:AVTransport:1\r\n"
      "USN: uuid:abc::urn:schemas-upnp-org:service:AVTransport:1\r\n"
      "\r\n",
      &r));
  CHECK(r.location == "http://192.168.1.20:8200/desc");
  CHECK(r.st == cast::kAvTransportType);
  CHECK(r.usn == "uuid:abc::urn:schemas-upnp-org:service:AVTransport:1");

  // Lower-case header names and bare LF endings are both on the wire.
  CHECK(cast::parseSearchResponse(
      "HTTP/1.1 200 OK\nlocation: http://h/desc\nst: st-target\nusn: u\n\n", &r));
  CHECK(r.location == "http://h/desc");
  CHECK(r.st == "st-target");

  // A NOTIFY announcement carries NT, never ST: not a search reply.
  CHECK_FALSE(cast::parseSearchResponse(
      "NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n"
      "NT: urn:schemas-upnp-org:service:AVTransport:1\r\n"
      "USN: uuid:x\r\nLOCATION: http://h/desc\r\n\r\n",
      &r));
  // Missing LOCATION: the reply cannot be followed.
  CHECK_FALSE(cast::parseSearchResponse(
      "HTTP/1.1 200 OK\r\nST: st\r\nUSN: u\r\n\r\n", &r));
  // Missing USN: no dedup key.
  CHECK_FALSE(cast::parseSearchResponse(
      "HTTP/1.1 200 OK\r\nST: st\r\nLOCATION: http://h\r\n\r\n", &r));
  // An empty LOCATION value cannot be followed either.
  CHECK_FALSE(cast::parseSearchResponse(
      "HTTP/1.1 200 OK\r\nST: st\r\nUSN: u\r\nLOCATION: \r\n\r\n", &r));
  CHECK_FALSE(cast::parseSearchResponse("not http at all\r\n\r\n", &r));
}

TEST_CASE("field lookup is case-insensitive and trims like the wire forms") {
  const std::string head =
      "HTTP/1.1 500 Internal Server Error\r\nServer: Test/1\r\nCONTENT-LENGTH: 41\r\n\r\n";
  CHECK(cast::fieldValue(head, "content-length") == "41");
  CHECK(cast::fieldValue(head, "SERVER") == "Test/1");
  CHECK(cast::fieldValue(head, "Missing").empty());
  // The status line's colon (none here, but "HTTP/1.1 200: x" would be
  // one) never reads as a header name.
  CHECK(cast::fieldValue("HTTP/1.1 200 OK\r\na: b\r\n", "HTTP/1.1 200 OK").empty());
  CHECK(cast::ciEqual("ControlURL", "controlurl"));
  CHECK_FALSE(cast::ciEqual("abc", "abcd"));
  CHECK(cast::lower("MixedCase") == "mixedcase");
}

TEST_CASE("tag extraction matches case-insensitively and decodes entities") {
  const std::string xml = "<Root><friendlyName>Living &amp; Room</friendlyName>"
                          "<CONTROLURL>/a?x=1&amp;y=2</CONTROLURL>"
                          "<empty></empty><selfclosed/>"
                          "<multi>one</multi><multi>two</multi></Root>";
  CHECK(cast::tagText(xml, "friendlyName") == "Living & Room");
  CHECK(cast::tagText(xml, "controlURL") == "/a?x=1&y=2");
  CHECK(cast::tagText(xml, "empty").empty());
  CHECK(cast::tagText(xml, "absent").empty());
  // Attributes on the opening tag are tolerated.
  CHECK(cast::tagText("<Track val=\"1\">7</Track>", "Track") == "7");
  const std::vector<std::string> many = cast::tagTexts(xml, "multi");
  CHECK(many.size() == 2);
  CHECK(many[0] == "one");
  CHECK(many[1] == "two");
  // An unterminated tag is not a value.
  CHECK(cast::tagText("<a>no close", "a").empty());

  CHECK(cast::xmlDecode("a&lt;b&gt;c&quot;d&apos;e&amp;f&#65;&#x42;g") == "a<b>c\"d'e&fABg");
  // An undefined escape and a lone '&' survive verbatim.
  CHECK(cast::xmlDecode("x&nope;y & z") == "x&nope;y & z");
  CHECK(cast::xmlEscape("a<b>c\"d'e&f") == "a&lt;b&gt;c&quot;d&apos;e&amp;f");
}

TEST_CASE("attribute extraction reads the GENA state reports") {
  // The transport state rides attributes, not element text.
  CHECK(cast::tagAttr("<LastChange val=\"PLAYING\"/>", "LastChange", "val") == "PLAYING");
  // Single quotes, spacing variants, and case-insensitive names.
  CHECK(cast::tagAttr("<lastchange VAL = 'PAUSED_PLAYBACK' />", "LastChange",
                      "val") == "PAUSED_PLAYBACK");
  // Entities inside the value decode; the tag name never parses as an
  // attribute name, and namespace-qualified attributes are skipped whole.
  CHECK(cast::tagAttr("<e:property xmlns:e=\"urn:x\"><LastChange val=\"a&lt;b&quot;c\"/>"
                      "</e:property>",
                      "LastChange", "val") == "a<b\"c");
  CHECK(cast::tagAttr("<LastChange val=\"PLAYING\"/>", "LastChange", "absent").empty());
  CHECK(cast::tagAttr("<Other val=\"PLAYING\"/>", "LastChange", "val").empty());
  CHECK(cast::tagAttr("no tags at all", "LastChange", "val").empty());
  // An unterminated value is not a value.
  CHECK(cast::tagAttr("<LastChange val=\"PLAYING>", "LastChange", "val").empty());
}

TEST_CASE("device descriptions parse and resolve the AVTransport control URL") {
  // The common Samsung/LG-shaped layout: relative control URLs resolved
  // against the description URL, several services beside AVTransport.
  const std::string desc =
      "<?xml version=\"1.0\"?>\n"
      "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">\n"
      "  <device>\n"
      "    <deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>\n"
      "    <friendlyName>Living Room TV</friendlyName>\n"
      "    <UDN>uuid:16b4cbe5-fake-udn&amp;more</UDN>\n"
      "    <serviceList>\n"
      "      <service>\n"
      "        <serviceType>urn:schemas-upnp-org:service:RenderingControl:1</serviceType>\n"
      "        <serviceId>urn:upnp-org:serviceId:RenderingControl</serviceId>\n"
      "        <controlURL>/RenderingControl/ctrl</controlURL>\n"
      "        <eventSubURL>/RenderingControl/evt</eventSubURL>\n"
      "      </service>\n"
      "      <service>\n"
      "        <serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType>\n"
      "        <serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>\n"
      "        <controlURL>/AVTransport/ctrl</controlURL>\n"
      "        <eventSubURL>/AVTransport/evt</eventSubURL>\n"
      "      </service>\n"
      "    </serviceList>\n"
      "  </device>\n"
      "</root>";
  cast::DeviceDesc device;
  REQUIRE(cast::parseDeviceDescription(desc, &device));
  CHECK(device.friendly_name == "Living Room TV");
  CHECK(device.device_type == "urn:schemas-upnp-org:device:MediaRenderer:1");
  CHECK(device.udn == "uuid:16b4cbe5-fake-udn&more");
  REQUIRE(device.services.size() == 2);
  CHECK(device.services[0].service_type.find("RenderingControl") != std::string::npos);
  CHECK(device.services[1].control_url == "/AVTransport/ctrl");
  CHECK(device.services[1].eventsub_url == "/AVTransport/evt");

  std::string url;
  REQUIRE(cast::avTransportControlUrl(device, "http://192.168.1.20:8200/desc.xml", &url));
  CHECK(url == "http://192.168.1.20:8200/AVTransport/ctrl");

  // The event subscription URL resolves the same way.
  std::string event_url;
  REQUIRE(cast::avTransportEventUrl(device, "http://192.168.1.20:8200/desc.xml",
                                    &event_url));
  CHECK(event_url == "http://192.168.1.20:8200/AVTransport/evt");

  // An absolute control URL passes through untouched.
  cast::DeviceDesc absolute = device;
  absolute.services[1].control_url = "http://other-host:9/ctrl";
  REQUIRE(cast::avTransportControlUrl(absolute, "http://192.168.1.20:8200/d", &url));
  CHECK(url == "http://other-host:9/ctrl");

  // A service type too short to carry the prefix is skipped, not matched.
  cast::DeviceDesc short_type;
  short_type.services.push_back({"urn:avtransport", "id", "/ctrl", "/evt"});
  CHECK_FALSE(cast::avTransportControlUrl(short_type, "http://h/d", &url));
  // An AVTransport service without a usable control URL defers to the
  // next matching service.
  cast::DeviceDesc two;
  two.services.push_back(
      {"urn:schemas-upnp-org:service:AVTransport:1", "id", "", "/evt"});
  two.services.push_back(
      {"urn:schemas-upnp-org:service:AVTransport:1", "id2", "/ctrl2", "/evt"});
  REQUIRE(cast::avTransportControlUrl(two, "http://h/d", &url));
  CHECK(url == "http://h/ctrl2");

  // A device without AVTransport parses fine but has no control URL.
  cast::DeviceDesc no_av;
  no_av.services.push_back({"urn:schemas-upnp-org:service:RenderingControl:1", "id",
                            "/ctrl", "/evt"});
  CHECK_FALSE(cast::avTransportControlUrl(no_av, "http://h/d", &url));
  CHECK_FALSE(cast::avTransportEventUrl(no_av, "http://h/d", &event_url));
  // A service without an eventSubURL leaves the session poll-only.
  cast::DeviceDesc no_evt;
  no_evt.services.push_back(
      {"urn:schemas-upnp-org:service:AVTransport:1", "id", "/ctrl", ""});
  CHECK_FALSE(cast::avTransportEventUrl(no_evt, "http://h/d", &event_url));
  // Not a device description at all.
  cast::DeviceDesc junk;
  CHECK_FALSE(cast::parseDeviceDescription("<html>404</html>", &junk));
  CHECK_FALSE(cast::parseDeviceDescription("plain text body", &junk));
  // A description without a serviceList parses true, zero services.
  cast::DeviceDesc bare;
  CHECK(cast::parseDeviceDescription(
      "<root><device><friendlyName>Bare</friendlyName></device></root>", &bare));
  CHECK(bare.services.empty());
}

TEST_CASE("URL splitting and reference resolution follow the device forms") {
  cast::UrlParts parts;
  REQUIRE(cast::splitUrl("http://192.168.1.20:8200/AVTransport/ctrl", &parts));
  CHECK(parts.host == "192.168.1.20");
  CHECK(parts.port == 8200);
  CHECK(parts.path == "/AVTransport/ctrl");
  // Default port and bare origin.
  REQUIRE(cast::splitUrl("http://tv.local", &parts));
  CHECK(parts.port == 80);
  CHECK(parts.path.empty());
  REQUIRE(cast::splitUrl("http://10.0.0.3:7676", &parts));
  CHECK(parts.port == 7676);
  CHECK(parts.path.empty());
  // Rejections: wrong scheme, no host, bad port.
  CHECK_FALSE(cast::splitUrl("https://tv/ctrl", &parts));
  CHECK_FALSE(cast::splitUrl("http:///ctrl", &parts));
  CHECK_FALSE(cast::splitUrl("http://tv:notaport/ctrl", &parts));
  CHECK_FALSE(cast::splitUrl("http://tv:99999/ctrl", &parts));
  CHECK_FALSE(cast::splitUrl("no scheme at all", &parts));

  const std::string base = "http://192.168.1.20:8200/desc/device.xml";
  CHECK(cast::resolveUrl(base, "/ctrl") == "http://192.168.1.20:8200/ctrl");
  CHECK(cast::resolveUrl(base, "ctrl") == "http://192.168.1.20:8200/desc/ctrl");
  CHECK(cast::resolveUrl(base, "ctrl?x=1") == "http://192.168.1.20:8200/desc/ctrl?x=1");
  CHECK(cast::resolveUrl(base, "http://other/x") == "http://other/x");
  CHECK(cast::resolveUrl(base, "") .empty());
  // The base's own query never carries into a resolved path.
  CHECK(cast::resolveUrl("http://h/d.xml?sid=9", "ctrl") == "http://h/ctrl");
  // No origin to hang a reference on.
  CHECK(cast::resolveUrl("not a url", "ctrl").empty());
  // Absolute references lose their fragment; a pathless base merges
  // directly onto the authority.
  CHECK(cast::resolveUrl(base, "http://x/y#frag") == "http://x/y");
  CHECK(cast::resolveUrl("http://h", "rel") == "http://hrel");

  // Bracketed IPv6 literals: host, default port, explicit port.
  REQUIRE(cast::splitUrl("http://[::1]:8200/x", &parts));
  CHECK(parts.host == "::1");
  CHECK(parts.port == 8200);
  CHECK(parts.path == "/x");
  REQUIRE(cast::splitUrl("http://[::1]", &parts));
  CHECK(parts.host == "::1");
  CHECK(parts.port == 80);
  // Malformed bracket forms are refused.
  CHECK_FALSE(cast::splitUrl("http://[::1]junk", &parts));
  CHECK_FALSE(cast::splitUrl("http://[::1", &parts));
  // Shorter than the scheme.
  CHECK_FALSE(cast::splitUrl("http:/x", &parts));
}

TEST_CASE("SOAP requests carry the envelope and an exact content length") {
  const std::string url = "http://192.168.1.20:8200/AVTransport/ctrl";
  auto body_of = [](const std::string& req) {
    return req.substr(req.find("\r\n\r\n") + 4);
  };

  // SetAVTransportURI: URI and title are XML-escaped, the DIDL metadata
  // repeats the URI, and the declared length matches the real body (the
  // renderer-side parser depends on that exactness).
  const std::string set = cast::setAvTransportUri(
      url, "http://127.0.0.1:44401/", "Big & Small <Media>");
  CHECK(set.substr(0, 29) == "POST /AVTransport/ctrl HTTP/1");
  CHECK(set.find("HOST: 192.168.1.20:8200\r\n") != std::string::npos);
  CHECK(set.find("SOAPACTION: \"urn:schemas-upnp-org:service:AVTransport:1#SetAVTransportURI\"") !=
        std::string::npos);
  CHECK(set.find("<CurrentURI>http://127.0.0.1:44401/</CurrentURI>") != std::string::npos);
  // The DIDL metadata is itself XML, so on the wire it is escaped inside
  // CurrentURIMetaData — the title appears double-escaped, ampersands
  // first by the DIDL content then by the envelope.
  CHECK(set.find("&lt;dc:title&gt;Big &amp;amp; Small &amp;lt;Media&amp;gt;&lt;/dc:title&gt;") !=
        std::string::npos);
  CHECK(set.find("&lt;res&gt;http://127.0.0.1:44401/&lt;/res&gt;") != std::string::npos);
  CHECK(set.find("DIDL-Lite") != std::string::npos);
  {
    const std::string body = body_of(set);
    const size_t at = set.find("CONTENT-LENGTH: ") + 16;
    const size_t declared = std::strtoull(set.c_str() + at, nullptr, 10);
    CHECK(declared == body.size());
  }

  // Play / Stop / Seek pin the action argument shapes.
  const std::string play = cast::play(url);
  CHECK(play.find("SOAPACTION: \"urn:schemas-upnp-org:service:AVTransport:1#Play\"") !=
        std::string::npos);
  CHECK(play.find("<u:Play xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">") !=
        std::string::npos);
  CHECK(play.find("<InstanceID>0</InstanceID><Speed>1</Speed>") != std::string::npos);

  // Pause takes no arguments past the instance id — no Speed, no Target.
  // Resume is not a separate action; it reuses Play at speed 1.
  const std::string pause = cast::pause(url);
  CHECK(pause.find("SOAPACTION: \"urn:schemas-upnp-org:service:AVTransport:1#Pause\"") !=
        std::string::npos);
  CHECK(pause.find("<u:Pause xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">") !=
        std::string::npos);
  CHECK(pause.find("<InstanceID>0</InstanceID>") != std::string::npos);
  CHECK(pause.find("Speed") == std::string::npos);
  CHECK(pause.find("<Speed>") == std::string::npos);
  {
    const std::string body = body_of(pause);
    const size_t at = pause.find("CONTENT-LENGTH: ") + 16;
    const size_t declared = std::strtoull(pause.c_str() + at, nullptr, 10);
    CHECK(declared == body.size());
  }

  const std::string stop = cast::stop(url);
  CHECK(stop.find("SOAPACTION: \"urn:schemas-upnp-org:service:AVTransport:1#Stop\"") !=
        std::string::npos);
  CHECK(stop.find("<u:Stop xmlns:u=") != std::string::npos);

  const std::string seek = cast::seekRelTime(url, 90000);
  CHECK(seek.find("<Unit>REL_TIME</Unit><Target>0:01:30</Target>") != std::string::npos);
  CHECK(seek.find("#Seek") != std::string::npos);

  const std::string poll = cast::getPositionInfo(url);
  CHECK(poll.find("#GetPositionInfo") != std::string::npos);

  // A URL that does not split still yields a well-formed request (the
  // HTTP layer is what rejects it later); the HOST header carries the
  // verbatim remainder.
  const std::string odd = cast::stop("not-a-url");
  CHECK(odd.substr(0, 11) == "POST / HTTP");
  CHECK(odd.find("HOST: not-a-url\r\n") != std::string::npos);
  CHECK(odd.find("#Stop") != std::string::npos);
}

TEST_CASE("parsers refuse null outputs and malformed clocks") {
  cast::SoapFault fault;
  cast::PositionInfo info;
  CHECK_FALSE(cast::parseFault("<errorCode>1</errorCode>", nullptr));
  CHECK_FALSE(cast::parsePositionInfo("<Track>1</Track>", nullptr));
  (void)fault;
  (void)info;

  // Fraction edges: empty fraction, non-numeric fraction, over-precise
  // fraction, missing hours.
  CHECK(cast::parseClockMs("0:00:00.") == -1);
  CHECK(cast::parseClockMs("0:00:00.x") == -1);
  CHECK(cast::parseClockMs("0:00:00.1234") == 123);
  CHECK(cast::parseClockMs(":00:00") == -1);
}

TEST_CASE("SOAP faults and position reports parse; clocks convert both ways") {
  cast::SoapFault fault;
  CHECK(cast::parseFault(
      "<s:Envelope><s:Body><s:Fault><detail><UPnPError "
      "xmlns=\"urn:schemas-upnp-org:control-1-0\"><errorCode>716</errorCode>"
      "<errorDescription>Resource not found</errorDescription></UPnPError>"
      "</detail></s:Fault></s:Body></s:Envelope>",
      &fault));
  CHECK(fault.code == 716);
  CHECK(fault.description == "Resource not found");
  // A plain 500 body is not a fault.
  CHECK_FALSE(cast::parseFault("<html>oops</html>", &fault));

  cast::PositionInfo info;
  CHECK(cast::parsePositionInfo(
      "<u:GetPositionInfoResponse "
      "xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
      "<Track>1</Track><TrackDuration>0:01:30</TrackDuration>"
      "<TrackMetaData></TrackMetaData><TrackURI>http://x/</TrackURI>"
      "<RelTime>0:00:07</RelTime><AbsTime>0:00:07</AbsTime>"
      "</u:GetPositionInfoResponse>",
      &info));
  CHECK(info.track == 1);
  CHECK(info.duration == "0:01:30");
  CHECK(info.rel_time == "0:00:07");
  // Not a position report at all.
  CHECK_FALSE(cast::parsePositionInfo("<html>x</html>", &info));

  CHECK(cast::parseClockMs("0:00:00") == 0);
  CHECK(cast::parseClockMs("0:00:07") == 7000);
  CHECK(cast::parseClockMs("1:02:03") == 3723000);
  // Fractions pad to milliseconds, keeping sub-second position fidelity.
  CHECK(cast::parseClockMs("01:02:03.5") == 3723500);
  CHECK(cast::parseClockMs("0:00:00.120") == 120);
  CHECK(cast::parseClockMs("0:00:01.25") == 1250);
  CHECK(cast::parseClockMs("0:00:00.999") == 999);
  // Renderers legally answer NOT_IMPLEMENTED: "unknown", never zero.
  CHECK(cast::parseClockMs("NOT_IMPLEMENTED") == -1);
  CHECK(cast::parseClockMs("") == -1);
  CHECK(cast::parseClockMs("garbage") == -1);
  CHECK(cast::parseClockMs("1:2") == -1);
  CHECK(cast::formatClockMs(3723000) == "1:02:03");
  CHECK(cast::formatClockMs(0) == "0:00:00");
  CHECK(cast::formatClockMs(-5) == "0:00:00");
  CHECK(cast::formatClockMs(90000) == "0:01:30");
}

TEST_CASE("GENA requests carry CALLBACK/NT/TIMEOUT and address the SID after") {
  const std::string sub =
      cast::subscribeRequest("http://192.168.1.20:8200/AVTransport/evt",
                             "http://192.168.1.5:41000/cast/events", 1800);
  CHECK(sub.find("SUBSCRIBE /AVTransport/evt HTTP/1.1\r\n") == 0);
  CHECK(sub.find("HOST: 192.168.1.20:8200\r\n") != std::string::npos);
  CHECK(sub.find("CALLBACK: <http://192.168.1.5:41000/cast/events>\r\n") !=
        std::string::npos);
  CHECK(sub.find("NT: upnp:event\r\n") != std::string::npos);
  CHECK(sub.find("TIMEOUT: Second-1800\r\n") != std::string::npos);
  // The requested timeout clamps to the spec's sane range.
  CHECK(cast::subscribeRequest("http://h/evt", "http://me/evt", 1)
            .find("TIMEOUT: Second-300\r\n") != std::string::npos);
  CHECK(cast::subscribeRequest("http://h/evt", "http://me/evt", 999999)
            .find("TIMEOUT: Second-86400\r\n") != std::string::npos);

  // Renewal and teardown carry the SID — and neither may re-announce
  // CALLBACK or NT, which the spec reserves for the initial subscribe.
  const std::string renew =
      cast::renewRequest("http://h:80/AVTransport/evt", "uuid:soar-evt-1", 1800);
  CHECK(renew.find("SUBSCRIBE /AVTransport/evt HTTP/1.1\r\n") == 0);
  CHECK(renew.find("SID: uuid:soar-evt-1\r\n") != std::string::npos);
  CHECK(renew.find("CALLBACK") == std::string::npos);
  CHECK(renew.find("NT:") == std::string::npos);
  const std::string bye =
      cast::unsubscribeRequest("http://h/AVTransport/evt", "uuid:soar-evt-1");
  CHECK(bye.find("UNSUBSCRIBE /AVTransport/evt HTTP/1.1\r\n") == 0);
  CHECK(bye.find("SID: uuid:soar-evt-1\r\n") != std::string::npos);
  CHECK(bye.find("CALLBACK") == std::string::npos);
}

TEST_CASE("subscription replies parse only when SID and 200 agree") {
  cast::Subscription sub;
  REQUIRE(cast::parseSubscribeReply(
      "HTTP/1.1 200 OK\r\nSID: uuid:soar-evt-1\r\nTIMEOUT: Second-1800\r\n"
      "SERVER: x\r\n\r\n",
      &sub));
  CHECK(sub.sid == "uuid:soar-evt-1");
  CHECK(sub.timeout_secs == 1800);
  // "infinite" is the spec's no-expiry form; case-insensitive.
  REQUIRE(cast::parseSubscribeReply(
      "HTTP/1.1 200 OK\r\nsid: uuid:other\r\ntimeout: Second-infinite\r\n\r\n",
      &sub));
  CHECK(sub.sid == "uuid:other");
  CHECK(sub.timeout_secs == 0);
  // A refused subscribe is not a subscription; neither is a 200 without
  // a SID.
  CHECK_FALSE(cast::parseSubscribeReply(
      "HTTP/1.1 412 Precondition Failed\r\nContent-Length: 0\r\n\r\n", &sub));
  CHECK_FALSE(cast::parseSubscribeReply(
      "HTTP/1.1 200 OK\r\nTIMEOUT: Second-1800\r\n\r\n", &sub));
}

// The real NOTIFY wire form: the state rides the escaped LastChange
// attribute of a property-set body.
std::string notifyBody(const char* state) {
  const std::string inner =
      std::string("<Event xmlns=\"urn:schemas-upnp-org:event-1-0\">"
                  "<InstanceID val=\"0\"><TransportState val=\"") +
      state + "\"/></InstanceID></Event>";
  return "<e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\">"
         "<e:property><LastChange val=\"" +
         cast::xmlEscape(inner) + "\"/></e:property></e:propertyset>";
}

TEST_CASE("NOTIFY bodies report the transport state they carry") {
  CHECK(cast::parseNotifyState(notifyBody("PLAYING")) == cast::TransportState::Playing);
  CHECK(cast::parseNotifyState(notifyBody("PAUSED_PLAYBACK")) ==
        cast::TransportState::PausedPlayback);
  CHECK(cast::parseNotifyState(notifyBody("STOPPED")) == cast::TransportState::Stopped);
  CHECK(cast::parseNotifyState(notifyBody("TRANSITIONING")) ==
        cast::TransportState::Transitioning);
  CHECK(cast::parseNotifyState(notifyBody("NO_MEDIA_PRESENT")) ==
        cast::TransportState::NoMediaPresent);
  // An unparseable body is Unknown, never a state guess.
  CHECK(cast::parseNotifyState("<e:propertyset/>") == cast::TransportState::Unknown);
  CHECK(cast::parseNotifyState("not xml") == cast::TransportState::Unknown);
  CHECK(cast::parseNotifyState(notifyBody("SOMETHING_ELSE")) ==
        cast::TransportState::Unknown);
}

TEST_CASE("HTTP status extraction") {
  CHECK(cast::httpStatus("HTTP/1.1 200 OK\r\nX: 1\r\n\r\nbody") == 200);
  CHECK(cast::httpStatus("HTTP/1.0 500 Internal Server Error\r\n\r\n") == 500);
  CHECK(cast::httpStatus("HTTP/1.1 206 Partial Content\r\n\r\n") == 206);
  CHECK(cast::httpStatus("NOT-HTTP\r\n\r\n") == 0);
  CHECK(cast::httpStatus("HTTP/1.1 abc\r\n") == 0);
  CHECK(cast::httpStatus("HTTP/1.1") == 0);
  // Truncated or non-numeric status codes.
  CHECK(cast::httpStatus("HTTP/1.1 20\r\n") == 0);
  CHECK(cast::httpStatus("HTTP/1.1 20x\r\n") == 0);
  CHECK(cast::httpStatus("HTTP/") == 0);
}

#ifdef _WIN32

TEST_CASE("cast smoke cases are POSIX-only") {
  MESSAGE("Windows: the in-process renderer and the bridge rebind cases "
          "are POSIX sockets here (the p2p suite's gate); the pure string "
          "cases above ran everywhere");
}

#else

// ---------------------------------------------------------------------------
// Socket half — a minimal in-process MediaRenderer, POSIX only.
// ---------------------------------------------------------------------------

namespace {

std::string makeTempDir(const char* tag) {
  std::string tmpl = std::string("/tmp/soar_cast_") + tag + "_XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  const char* dir = ::mkdtemp(buf.data());
  REQUIRE(dir != nullptr);
  return std::string(dir);
}

std::string patternBytes(size_t n) {
  std::string s;
  s.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    s.push_back(static_cast<char>((i * 31 + i % 7) & 0xff));
  }
  return s;
}

void writeBytes(const std::string& path, const std::string& bytes) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  REQUIRE(f.good());
  f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  REQUIRE(f.good());
}

std::string buildTorrent(const std::string& root, const std::string& out) {
  struct stat st {};
  REQUIRE(::stat((root + "/media.bin").c_str(), &st) == 0);
  lt::file_storage fs;
  fs.add_file("media.bin", static_cast<std::int64_t>(st.st_size));
  lt::create_torrent ct(fs, 0, lt::create_torrent::v1_only);
  ct.set_creator("soar-test_cast/1");
  lt::set_piece_hashes(ct, root, [](lt::piece_index_t) {});
  std::vector<char> buf;
  lt::bencode(std::back_inserter(buf), ct.generate());
  std::ofstream f(out, std::ios::binary | std::ios::trunc);
  REQUIRE(f.good());
  f.write(buf.data(), static_cast<std::streamsize>(buf.size()));
  REQUIRE(f.good());
  return out;
}

// One self-seeded one-file torrent: the store is pre-seeded, the bridge
// serves without any swarm.
struct SoloStore {
  std::string dir;
  std::string torrent;
  std::string bytes;
};

SoloStore makeSolo(const char* tag) {
  SoloStore s;
  s.dir = makeTempDir(tag);
  s.bytes = patternBytes(1 << 20);
  writeBytes(s.dir + "/media.bin", s.bytes);
  s.torrent = buildTorrent(s.dir, s.dir + "/media.torrent");
  return s;
}

int getStatus(const std::string& url) {
  // "http://<host>:<port>/" from playbackUrl().
  const size_t colon = url.rfind(':');
  REQUIRE(colon != std::string::npos);
  const size_t host_at = url.find("//");
  REQUIRE(host_at != std::string::npos);
  const std::string host = url.substr(host_at + 2, colon - host_at - 2);
  const int port = std::atoi(url.c_str() + colon + 1);
  REQUIRE(port > 0);

  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(fd >= 0);
  timeval tv{};
  tv.tv_sec = 10;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<unsigned short>(port));
  REQUIRE(::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) == 1);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return 0;
  }
  const std::string req =
      "GET / HTTP/1.1\r\nHost: " + host +
      "\r\nRange: bytes=0-1023\r\nConnection: close\r\n\r\n";
  size_t off = 0;
  while (off < req.size()) {
    const ssize_t w = ::send(fd, req.data() + off, req.size() - off, 0);
    if (w <= 0) break;
    off += static_cast<size_t>(w);
  }
  char buf[256] = {0};
  ssize_t got = 0, total = 0;
  while (total < 200 &&
         (got = ::recv(fd, buf + total, sizeof(buf) - static_cast<size_t>(total) - 1, 0)) > 0) {
    total += got;
    if (std::strstr(buf, "\r\n\r\n") != nullptr) break;
  }
  ::close(fd);
  // "HTTP/1.x NNN ..."
  const char* sp = std::strchr(buf, ' ');
  return sp != nullptr ? std::atoi(sp + 1) : 0;
}

bool waitPhase(const cast::CastSession& session, cast::CastSession::Phase want,
               int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  while (session.phase() != want) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return true;
}

// What the fake renderer observed, shared with its (detached) connection
// threads so they never touch the owning object.
struct FakeState {
  bool fault_set_uri = false;
  bool fault_play = false;
  bool fault_pause = false;
  bool fault_seek = false;
  bool fault_no_desc = false;  // fault reply carries no errorDescription
  bool stalled = false;
  bool no_avtransport = false;
  bool no_clock = false;      // GetPositionInfo answers NOT_IMPLEMENTED
  bool go_deaf_after = 0;
  bool not_http = false;        // description replies with garbage
  bool huge_desc_length = false;  // description declares a giant length
  bool no_length = false;       // description without Content-Length
  bool spill = false;           // description declares a too-short length
  bool flood_head = false;      // description is a giant unterminated head
  bool empty_name = false;      // description has an empty friendlyName
  bool bad_control = false;     // controlURL scheme the control point rejects
  bool refuse_subscribe = false;  // SUBSCRIBE answers 412
  bool short_event_lease = false; // SUBSCRIBE grants Second-5, forcing renewal
  std::string description;

  std::mutex mu;
  std::string uri;
  std::string title;
  std::string didl;  // the unescaped DIDL metadata (protocolInfo assertions)
  std::string seek_target;
  bool played = false;
  bool stop_requested = false;
  bool pause_requested = false;
  int position_requests = 0;
  int media_fetch_status = 0;
  std::string callback_url;  // CALLBACK the control point subscribed with
  int subscribe_count = 0;
  int renew_count = 0;
  bool unsubscribed = false;
  bool playing = false;
  bool paused = false;
  std::chrono::steady_clock::time_point play_epoch{};
  std::int64_t seek_base_ms = 0;
  std::int64_t pause_base_ms = 0;  // clock frozen here while paused
};

std::string fakeClock(std::int64_t ms) {
  if (ms < 0) ms = 0;
  const std::int64_t total = ms / 1000;
  // Fractional seconds: the test polls at 120 ms, faster than a whole-
  // second clock would distinguish.
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%d:%02d:%02d.%03d", static_cast<int>(total / 3600),
                static_cast<int>((total / 60) % 60), static_cast<int>(total % 60),
                static_cast<int>(ms % 1000));
  return buf;
}

std::string headerValue(const std::string& head, const char* name) {
  std::string lower_head = head;
  for (char& c : lower_head) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  const std::string key = std::string("\r\n") + name;
  const size_t at = lower_head.find(key);
  if (at == std::string::npos) return "";
  size_t v = at + key.size();
  while (v < head.size() && (head[v] == ' ' || head[v] == '\t')) ++v;
  size_t e = head.find("\r\n", v);
  if (e == std::string::npos) e = head.size();
  return head.substr(v, e - v);
}

// The renderer's own fetch of the media URL the control point handed it —
// the proof that the bridge really served the cast source. Range 0-1023
// on a pre-seeded file: a 206 is the bridge's Range contract.
int fetchMediaStatus(const std::string& uri) {
  const size_t scheme = uri.find("//");
  if (scheme == std::string::npos) return 0;
  const size_t colon = uri.find(':', scheme + 2);
  if (colon == std::string::npos) return 0;
  const std::string host = uri.substr(scheme + 2, colon - scheme - 2);
  const int port = std::atoi(uri.c_str() + colon + 1);
  if (port <= 0) return 0;
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return 0;
  timeval tv{};
  tv.tv_sec = 10;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<unsigned short>(port));
  // No doctest asserts here: this runs on a detached connection thread,
  // where the framework's context does not exist.
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    ::close(fd);
    return 0;
  }
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return 0;
  }
  const std::string req = "GET / HTTP/1.1\r\nHost: " + host +
                          "\r\nRange: bytes=0-1023\r\nConnection: close\r\n\r\n";
  ::send(fd, req.data(), req.size(), 0);
  char buf[256] = {0};
  ::recv(fd, buf, sizeof(buf) - 1, 0);
  ::close(fd);
  const char* sp = std::strchr(buf, ' ');
  return sp != nullptr ? std::atoi(sp + 1) : 0;
}

// Serves one connection: a read of the request (head + body), then the
// scripted answer. Runs detached, holding only the shared state.
void serveConn(std::shared_ptr<FakeState> state, int fd) {
  timeval tv{};
  tv.tv_sec = 5;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  std::string buf;
  for (;;) {
    const size_t head_end = buf.find("\r\n\r\n");
    if (head_end == std::string::npos) {
      char tmp[4096];
      const ssize_t r = ::recv(fd, tmp, sizeof(tmp), 0);
      if (r <= 0) break;
      buf.append(tmp, static_cast<size_t>(r));
      continue;
    }
    std::string head = buf.substr(0, head_end);
    std::string rest = buf.substr(head_end + 4);
    // Body by Content-Length.
    const std::string length_field = headerValue(head, "content-length:");
    if (!length_field.empty()) {
      const size_t want = static_cast<size_t>(std::strtoull(length_field.c_str(), nullptr, 10));
      while (rest.size() < want) {
        char tmp[4096];
        const ssize_t r = ::recv(fd, tmp, sizeof(tmp), 0);
        if (r <= 0) break;
        rest.append(tmp, static_cast<size_t>(r));
      }
    }
    buf.clear();
    if (rest.size() > 4) buf = rest.substr(4);  // unlikely pipelined extra

    const size_t sp1 = head.find(' ');
    const std::string path = sp1 == std::string::npos
                                 ? ""
                                 : head.substr(sp1 + 1, head.find(' ', sp1 + 1) - sp1 - 1);

    std::string reply;
    if (path.rfind("/desc.xml", 0) == 0) {
      if (state->not_http) {
        ::send(fd, "GARBAGE\r\n\r\n", 11, 0);
        break;
      }
      if (state->flood_head) {
        // 5 MiB of head with no terminator: the client must cut at its
        // wire budget instead of buffering forever.
        const std::string chunk(65536, 'a');
        ::send(fd, "HTTP/1.1 200 OK\r\n", 17, 0);
        for (int i = 0; i < 80; ++i) {
          if (::send(fd, chunk.data(), chunk.size(), 0) < 0) break;
        }
        break;
      }
      if (state->huge_desc_length) {
        // A declared length beyond the control point's wire budget: the
        // client must cut before reading any body.
        reply = "HTTP/1.1 200 OK\r\nContent-Type: text/xml\r\nConnection: close\r\nContent-Length: 99999999\r\n\r\n";
        ::send(fd, reply.data(), reply.size(), 0);
        break;
      }
      if (state->no_length) {
        // No Content-Length: body read to the connection close.
        reply = "HTTP/1.1 200 OK\r\nContent-Type: text/xml\r\nConnection: close\r\n\r\n" + state->description;
        ::send(fd, reply.data(), reply.size(), 0);
        break;
      }
      if (state->spill) {
        // Declared shorter than sent: the client trims the spill.
        reply = "HTTP/1.1 200 OK\r\nContent-Type: text/xml\r\nConnection: close\r\nContent-Length: 10\r\n\r\n" + state->description;
        ::send(fd, reply.data(), reply.size(), 0);
        break;
      }
      reply = "HTTP/1.1 200 OK\r\nContent-Type: text/xml\r\nConnection: close\r\nContent-Length: " +
              std::to_string(state->description.size()) + "\r\n\r\n" + state->description;
      ::send(fd, reply.data(), reply.size(), 0);
      break;  // Connection: close
    }
    if (path.rfind("/AVTransport/evt", 0) == 0) {
      // GENA: SUBSCRIBE records the CALLBACK and answers a SID; a
      // subscription with a SID header is a renewal; UNSUBSCRIBE retires.
      const std::string method = head.substr(0, sp1);
      const std::string sid_hdr = headerValue(head, "sid:");
      if (method == "SUBSCRIBE") {
        bool refuse = false;
        if (sid_hdr.empty()) {
          std::string cb = headerValue(head, "callback:");
          if (cb.size() >= 2 && cb.front() == '<' && cb.back() == '>')
            cb = cb.substr(1, cb.size() - 2);
          std::lock_guard<std::mutex> lock(state->mu);
          state->callback_url = cb;
          ++state->subscribe_count;
          refuse = state->refuse_subscribe;
        } else {
          std::lock_guard<std::mutex> lock(state->mu);
          ++state->renew_count;
          refuse = state->refuse_subscribe;
        }
        const std::string reply =
            refuse
                ? std::string("HTTP/1.1 412 Precondition Failed\r\nContent-Length: "
                              "0\r\nConnection: close\r\n\r\n")
                : "HTTP/1.1 200 OK\r\nSID: uuid:soar-evt-1\r\nTIMEOUT: Second-" +
                      std::string(state->short_event_lease ? "5" : "1800") +
                      "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        ::send(fd, reply.data(), reply.size(), 0);
        break;
      }
      if (method == "UNSUBSCRIBE") {
        {
          std::lock_guard<std::mutex> lock(state->mu);
          state->unsubscribed = true;
        }
        const std::string bye =
            "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        ::send(fd, bye.data(), bye.size(), 0);
        break;
      }
      const std::string not_found =
          "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
      ::send(fd, not_found.data(), static_cast<int>(not_found.size()), 0);
      break;
    }
    if (path.rfind("/AVTransport/ctrl", 0) != 0) {
      const std::string not_found =
          "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
      ::send(fd, not_found.data(), static_cast<int>(not_found.size()), 0);
      break;
    }

    // SOAP action: the SOAPACTION header's fragment names it.
    std::string action = headerValue(head, "soapaction:");
    const size_t hash = action.find('#');
    action = hash == std::string::npos ? "" : action.substr(hash + 1);
    if (!action.empty() && action.back() == '"') action.pop_back();

    const std::string fault_body =
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
        "<s:Fault><detail><UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\">"
        "<errorCode>716</errorCode><errorDescription>Resource not found"
        "</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>";
    const std::string fault_body_no_desc =
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
        "<s:Fault><detail><UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\">"
        "<errorCode>401</errorCode></UPnPError></detail></s:Fault></s:Body>"
        "</s:Envelope>";
    const std::string ok_envelope =
        "<?xml version=\"1.0\"?><s:Envelope "
        "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
        "<s:Body><u:" + action + "Response xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
        "</u:" + action + "Response></s:Body></s:Envelope>";

    std::string body;
    if (action == "SetAVTransportURI") {
      std::lock_guard<std::mutex> lock(state->mu);
      state->uri = soar::cast::tagText(rest, "CurrentURI");
      // The DIDL metadata rides escaped inside CurrentURIMetaData; a real
      // renderer parses it after one unescape, then reads the title whose
      // text is escaped a second time.
      const std::string didl =
          soar::cast::xmlDecode(soar::cast::tagText(rest, "CurrentURIMetaData"));
      state->didl = didl;
      state->title = soar::cast::xmlDecode(soar::cast::tagText(didl, "dc:title"));
      if (state->fault_set_uri) {
        body = state->fault_no_desc ? fault_body_no_desc : fault_body;
      } else {
        // The renderer fetches what it was handed, before acknowledging.
        state->media_fetch_status = fetchMediaStatus(state->uri);
        body = ok_envelope;
      }
    } else if (action == "Play") {
      std::lock_guard<std::mutex> lock(state->mu);
      state->played = true;
      if (!state->fault_play) {
        state->playing = true;
        state->paused = false;
        // Resume continues from where Pause froze the clock: anchor the
        // epoch so elapsed picks up from pause_base_ms.
        const std::int64_t base_ms = state->playing ? state->pause_base_ms : 0;
        state->play_epoch =
            std::chrono::steady_clock::now() -
            std::chrono::milliseconds(base_ms);
      }
      body = state->fault_play ? fault_body : ok_envelope;
    } else if (action == "Pause") {
      std::lock_guard<std::mutex> lock(state->mu);
      state->pause_requested = true;
      if (!state->fault_pause) {
        const std::int64_t elapsed =
            state->playing
                ? std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - state->play_epoch).count() +
                  state->seek_base_ms
                : state->seek_base_ms;
        state->pause_base_ms = elapsed;  // freeze here
        state->paused = true;
        state->playing = false;
      }
      body = state->fault_pause ? fault_body : ok_envelope;
    } else if (action == "Stop") {
      std::lock_guard<std::mutex> lock(state->mu);
      state->stop_requested = true;
      state->playing = false;
      body = ok_envelope;
    } else if (action == "Seek") {
      std::lock_guard<std::mutex> lock(state->mu);
      state->seek_target = soar::cast::tagText(rest, "Target");
      if (state->fault_seek) {
        body = fault_body;  // rejected below with HTTP 500
      } else {
        // Jump the clock: parse H:MM:SS into the base offset.
        const std::string target = state->seek_target;
        const size_t c1 = target.find(':'), c2 = target.rfind(':');
        if (c1 != std::string::npos && c2 != std::string::npos) {
          state->seek_base_ms = (std::atoll(target.substr(0, c1).c_str()) * 3600 +
                                 std::atoll(target.substr(c1 + 1, c2 - c1 - 1).c_str()) * 60 +
                                 std::atoll(target.substr(c2 + 1).c_str())) * 1000;
          state->play_epoch = std::chrono::steady_clock::now();
        }
        body = ok_envelope;
      }
    } else if (action == "GetPositionInfo") {
      bool refuse = false;
      std::string rel;
      {
        std::lock_guard<std::mutex> lock(state->mu);
        ++state->position_requests;
        if (state->go_deaf_after != 0 &&
            state->position_requests > state->go_deaf_after) {
          refuse = true;
        } else if (state->stalled) {
          rel = "0:00:00";
        } else if (state->no_clock) {
          // A legal renderer answer: no parseable clock at all.
          rel = "NOT_IMPLEMENTED";
        } else {
          // While paused the clock holds at the freeze point, as a real
          // renderer reports it.
          const std::int64_t elapsed = state->playing
                ? std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - state->play_epoch).count() +
                  state->seek_base_ms
                : (state->paused ? state->pause_base_ms : state->seek_base_ms);
          rel = fakeClock(elapsed);
        }
      }
      if (refuse) break;  // deaf: close without answering
      body =
          "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
          "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
          "<u:GetPositionInfoResponse "
          "xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
          "<Track>1</Track><TrackDuration>0:01:00</TrackDuration>"
          "<TrackMetaData></TrackMetaData><TrackURI>x</TrackURI>"
          "<RelTime>" + rel + "</RelTime><AbsTime>" + rel + "</AbsTime>"
          "</u:GetPositionInfoResponse></s:Body></s:Envelope>";
    } else {
      const std::string server_error =
          "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n"
          "Connection: close\r\n\r\n";
      ::send(fd, server_error.data(), static_cast<int>(server_error.size()), 0);
      break;
    }

    const bool reject =
        (state->fault_set_uri && action == "SetAVTransportURI") ||
        (state->fault_play && action == "Play") ||
        (state->fault_pause && action == "Pause") ||
        (state->fault_seek && action == "Seek");
    if (!reject) {
      reply = "HTTP/1.1 200 OK\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nConnection: close\r\nContent-Length: " +
              std::to_string(body.size()) + "\r\n\r\n" + body;
    } else {
      reply = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nConnection: close\r\nContent-Length: " +
              std::to_string(body.size()) + "\r\n\r\n" + body;
    }
    ::send(fd, reply.data(), reply.size(), 0);
    break;  // Connection: close on every answer
  }
  ::close(fd);
}

// A minimal DMR: one description (relative control URL — the real
// resolution path), one AVTransport endpoint with a scripted position
// clock, plus an optional loopback multicast member answering M-SEARCH.
class FakeRenderer {
 public:
  struct Config {
    bool fault_set_uri = false;
    bool fault_play = false;
    bool fault_pause = false;
    bool fault_seek = false;
    bool fault_no_desc = false;
    bool stalled = false;
    bool no_avtransport = false;
    bool no_clock = false;
    bool not_http = false;
    bool huge_desc_length = false;
    bool no_length = false;
    bool spill = false;
    bool flood_head = false;
    bool empty_name = false;
    bool bad_control = false;
    bool refuse_subscribe = false;
    bool short_event_lease = false;
    int go_deaf_after = 0;
    bool join_group = false;  // answer loopback M-SEARCH bursts
  };

  explicit FakeRenderer(Config config) {
    state_ = std::make_shared<FakeState>();
    state_->fault_set_uri = config.fault_set_uri;
    state_->fault_play = config.fault_play;
    state_->fault_pause = config.fault_pause;
    state_->fault_seek = config.fault_seek;
    state_->fault_no_desc = config.fault_no_desc;
    state_->stalled = config.stalled;
    state_->no_avtransport = config.no_avtransport;
    state_->no_clock = config.no_clock;
    state_->not_http = config.not_http;
    state_->huge_desc_length = config.huge_desc_length;
    state_->no_length = config.no_length;
    state_->spill = config.spill;
    state_->flood_head = config.flood_head;
    state_->empty_name = config.empty_name;
    state_->bad_control = config.bad_control;
    state_->refuse_subscribe = config.refuse_subscribe;
    state_->short_event_lease = config.short_event_lease;
    state_->go_deaf_after = config.go_deaf_after;

    const char* av_service =
        "<service>"
        "<serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType>"
        "<serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>"
        "<controlURL>/AVTransport/ctrl</controlURL>"
        "<eventSubURL>/AVTransport/evt</eventSubURL>"
        "</service>";
    const char* av_service_ftp =
        "<service>"
        "<serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType>"
        "<serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>"
        "<controlURL>ftp://192.168.1.20/ctrl</controlURL>"
        "<eventSubURL>/AVTransport/evt</eventSubURL>"
        "</service>";
    state_->description =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">"
        "<device>"
        "<deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>"
        "<friendlyName>" +
        std::string(config.empty_name ? "" : "Soar Test Renderer") +
        "</friendlyName>"
        "<UDN>uuid:soar-test-renderer</UDN>"
        "<serviceList>"
        "<service>"
        "<serviceType>urn:schemas-upnp-org:service:RenderingControl:1</serviceType>"
        "<serviceId>urn:upnp-org:serviceId:RenderingControl</serviceId>"
        "<controlURL>/RenderingControl/ctrl</controlURL>"
        "<eventSubURL>/RenderingControl/evt</eventSubURL>"
        "</service>" +
        std::string(config.no_avtransport ? ""
                    : config.bad_control ? av_service_ftp : av_service) +
        "</serviceList>"
        "</device></root>";

    lfd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(lfd_ >= 0);
    int one = 1;
    ::setsockopt(lfd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    REQUIRE(::bind(lfd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    REQUIRE(::listen(lfd_, 8) == 0);
    sockaddr_in bound{};
    socklen_t blen = sizeof(bound);
    ::getsockname(lfd_, reinterpret_cast<sockaddr*>(&bound), &blen);
    port_ = ntohs(bound.sin_port);
    running_ = true;
    accept_thread_ = std::thread([this] { acceptLoop(); });

    if (config.join_group) {
      udp_fd_ = makeGroupSocket();
      REQUIRE(udp_fd_ >= 0);  // loud: membership is the discovery premise
      udp_thread_ = std::thread([this] { udpLoop(); });
    }
  }

  ~FakeRenderer() {
    running_ = false;
    // The listener retires like the product's stop(): shutdown, then close,
    // then join. Darwin ignores shutdown() on a listening socket — there
    // close() is what wakes the blocked accept; Linux wakes on the shutdown
    // and the close lands on an already-retired fd. Nothing opens an fd in
    // between (the UDP wake comes after the join), so the number cannot be
    // recycled under the waking thread. The UDP fd keeps the opposite order:
    // close vs a live recvfrom is a data race, so the datagram wakes the
    // loop first and the close lands after the join.
    if (lfd_ >= 0) ::shutdown(lfd_, SHUT_RDWR);
    if (lfd_ >= 0) ::close(lfd_);
    if (accept_thread_.joinable()) accept_thread_.join();
    if (udp_fd_ >= 0) wakeUdp();
    if (udp_thread_.joinable()) udp_thread_.join();
    if (udp_fd_ >= 0) ::close(udp_fd_);
  }

  void wakeUdp() {
    // A loopback datagram that is not an M-SEARCH: recvfrom returns, the
    // loop sees running_ == false and exits. The 3s SO_RCVTIMEO bounds the
    // join even if the datagram is lost.
    int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(1900);
    ::inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
    ::sendto(s, "soar-wake", 9, 0, reinterpret_cast<const sockaddr*>(&to),
             sizeof(to));
    ::close(s);
  }

  std::string location() const {
    return "http://127.0.0.1:" + std::to_string(port_) + "/desc.xml";
  }

  std::string lastUri() {
    std::lock_guard<std::mutex> lock(state_->mu);
    return state_->uri;
  }
  std::string lastTitle() {
    std::lock_guard<std::mutex> lock(state_->mu);
    return state_->title;
  }
  std::string lastDidl() {
    std::lock_guard<std::mutex> lock(state_->mu);
    return state_->didl;
  }
  std::string lastSeekTarget() {
    std::lock_guard<std::mutex> lock(state_->mu);
    return state_->seek_target;
  }
  bool sawPlay() {
    std::lock_guard<std::mutex> lock(state_->mu);
    return state_->played;
  }
  bool sawPause() {
    std::lock_guard<std::mutex> lock(state_->mu);
    return state_->pause_requested;
  }
  // Whether the renderer reports the clock frozen at the pause point.
  bool clockHeld() {
    std::lock_guard<std::mutex> lock(state_->mu);
    return state_->paused;
  }
  bool sawStop() {
    std::lock_guard<std::mutex> lock(state_->mu);
    return state_->stop_requested;
  }
  int mediaFetchStatus() {
    std::lock_guard<std::mutex> lock(state_->mu);
    return state_->media_fetch_status;
  }
  bool sawSubscribe() {
    std::lock_guard<std::mutex> lock(state_->mu);
    return state_->subscribe_count > 0;
  }
  int renewCount() {
    std::lock_guard<std::mutex> lock(state_->mu);
    return state_->renew_count;
  }
  bool sawUnsubscribed() {
    std::lock_guard<std::mutex> lock(state_->mu);
    return state_->unsubscribed;
  }

  // The renderer side of GENA: push a NOTIFY to the CALLBACK URL the
  // control point subscribed with. Returns the HTTP status answered
  // (0 when nothing was ever subscribed or the endpoint is unreachable).
  int pushNotify(const std::string& body) {
    std::string cb;
    {
      std::lock_guard<std::mutex> lock(state_->mu);
      cb = state_->callback_url;
    }
    // "http://127.0.0.1:<port>/cast/events"
    const size_t scheme = cb.find("//");
    if (scheme == std::string::npos) return 0;
    const size_t colon = cb.find(':', scheme + 2);
    const size_t slash = cb.find('/', scheme + 2);
    if (colon == std::string::npos || slash == std::string::npos) return 0;
    const int port = std::atoi(cb.c_str() + colon + 1);
    if (port <= 0) return 0;
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<unsigned short>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      ::close(fd);
      return 0;
    }
    const std::string req =
        "NOTIFY " + cb.substr(slash) + " HTTP/1.1\r\n"
        "HOST: 127.0.0.1\r\n"
        "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
        "NT: upnp:event\r\n"
        "NTS: upnp:propchange\r\n"
        "SID: uuid:soar-evt-1\r\n"
        "SEQ: 0\r\n"
        "CONTENT-LENGTH: " +
        std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n\r\n" +
        body;
    ::send(fd, req.data(), static_cast<int>(req.size()), 0);
    char buf[256] = {0};
    ::recv(fd, buf, sizeof(buf) - 1, 0);
    ::close(fd);
    const char* sp = std::strchr(buf, ' ');
    return sp != nullptr ? std::atoi(sp + 1) : 0;
  }

 private:
  static int makeGroupSocket() {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(1900);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      ::close(fd);
      return -1;
    }
    ip_mreq mreq{};
    REQUIRE(::inet_pton(AF_INET, "239.255.255.250", &mreq.imr_multiaddr) == 1);
    mreq.imr_interface.s_addr = htonl(INADDR_LOOPBACK);
    if (::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
      ::close(fd);
      return -1;
    }
    timeval tv{};
    tv.tv_sec = 3;  // bounds the thread's exit if close fails to wake recvfrom
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
  }

  void udpLoop() {
    while (running_) {
      char buf[2048];
      sockaddr_in from{};
      socklen_t flen = sizeof(from);
      const ssize_t r = ::recvfrom(udp_fd_, buf, sizeof(buf), 0,
                                   reinterpret_cast<sockaddr*>(&from), &flen);
      if (r <= 0) break;
      if (std::strstr(buf, "M-SEARCH") == nullptr) continue;
      const std::string reply =
          "HTTP/1.1 200 OK\r\n"
          "CACHE-CONTROL: max-age=1800\r\n"
          "LOCATION: " + location() + "\r\n"
          "SERVER: soar-fake/1 UPnP/1.0\r\n"
          "ST: urn:schemas-upnp-org:service:AVTransport:1\r\n"
          "USN: uuid:soar-test-renderer::urn:schemas-upnp-org:service:AVTransport:1\r\n"
          "\r\n";
      ::sendto(udp_fd_, reply.data(), static_cast<int>(reply.size()), 0,
               reinterpret_cast<const sockaddr*>(&from), flen);
    }
  }

  void acceptLoop() {
    while (running_) {
      const int cfd = static_cast<int>(::accept(lfd_, nullptr, nullptr));
      if (cfd < 0) break;  // woken by the destructor's shutdown/close
      std::thread(serveConn, state_, cfd).detach();
    }
  }

  std::shared_ptr<FakeState> state_;
  int lfd_ = -1;
  int udp_fd_ = -1;
  int port_ = 0;
  std::atomic<bool> running_{false};
  std::thread accept_thread_;
  std::thread udp_thread_;
};

struct TickCollector {
  std::mutex mu;
  std::vector<std::pair<std::int64_t, std::int64_t>> ticks;

  void operator()(std::int64_t pos, std::int64_t dur) {
    std::lock_guard<std::mutex> lock(mu);
    ticks.emplace_back(pos, dur);
  }
  size_t size() {
    std::lock_guard<std::mutex> lock(mu);
    return ticks.size();
  }
  bool strictlyIncreasingTail() {
    std::lock_guard<std::mutex> lock(mu);
    if (ticks.size() < 2) return false;
    for (size_t i = ticks.size() - 2; i + 1 < ticks.size(); ++i) {
      if (ticks[i + 1].first <= ticks[i].first) return false;
    }
    return true;
  }
  std::int64_t lastPos() {
    std::lock_guard<std::mutex> lock(mu);
    return ticks.empty() ? -1 : ticks.back().first;
  }
};

bool waitTicks(TickCollector& ticks, size_t want, int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  while (ticks.size() < want) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return true;
}

}  // namespace

// The live discovery smokes ride loopback multicast (INADDR_ANY:1900 +
// IP_ADD_MEMBERSHIP on lo0). Linux runs them everywhere; the macOS CI VM's
// lo0 multicast has never answered them (the run wedges instead of timing
// out cleanly), so there they are skipped — the pure-logic SSDP cases and
// the direct-location session cases still cover the parse and control
// planes on that platform. Windows is covered by the POSIX skip above.
#if !defined(__APPLE__)
TEST_CASE("discovery hears a loopback renderer and reads its description") {
  FakeRenderer::Config config;
  config.join_group = true;
  FakeRenderer renderer(config);

  std::vector<cast::DiscoveredRenderer> found;
  std::string err;
  const int n = cast::discoverRenderers(2500, &found, &err, "127.0.0.1");
  REQUIRE(n > 0);  // the join is the premise; silence after a clean join is a bug
  bool heard = false;
  for (const auto& d : found) {
    if (d.location == renderer.location()) heard = true;
  }
  CHECK(heard);
  // The friendly name rode along via the best-effort description fetch.
  CHECK(std::any_of(found.begin(), found.end(), [](const cast::DiscoveredRenderer& d) {
    return d.friendly_name == "Soar Test Renderer";
  }));

  // The timeout path: nobody new answers a second, deduplicated burst.
  std::vector<cast::DiscoveredRenderer> again;
  CHECK(cast::discoverRenderers(200, &again, &err, "127.0.0.1") >= 0);
}
#endif  // !defined(__APPLE__)

TEST_CASE("discovery and the client reject their degenerate inputs") {
  std::vector<cast::DiscoveredRenderer> found;
  std::string err;
  // No output vector: immediate refusal, no sockets needed.
  CHECK(cast::discoverRenderers(50, nullptr, &err) == -1);
  // A multicast interface that is not an address.
  CHECK(cast::discoverRenderers(50, &found, &err, "999.999.1.1") == -1);
  CHECK(err.find("bad multicast interface") != std::string::npos);

  // The client without a control URL refuses before any socket work.
  cast::RendererClient none("", cast::kAvTransportType);
  CHECK_FALSE(none.setUri("http://h/", "t", &err));
  CHECK(err == "cast: no control URL");
  CHECK_FALSE(none.pollPosition(nullptr, &err));
  CHECK(err == "cast: no control URL");
  // A control URL that cannot split is refused the same way.
  cast::RendererClient bad("not-a-url", cast::kAvTransportType);
  CHECK_FALSE(bad.setUri("http://h/", "t", &err));
  CHECK(err.find("cast: bad control URL") != std::string::npos);
}

TEST_CASE("a renderer without an AVTransport service fails the session by name") {
  const SoloStore store = makeSolo("noav");
  p2p::TorrentStream bridge;
  p2p::TorrentStream::Params bp;
  bp.torrent_path = store.torrent;
  bp.store_dir = store.dir;
  REQUIRE(bridge.start(bp));

  FakeRenderer::Config config;
  config.no_avtransport = true;
  FakeRenderer renderer(config);

  cast::CastSession session;
  cast::CastSession::Params params;
  params.bridge = &bridge;
  params.renderer_location = renderer.location();
  CHECK_FALSE(session.start(params));
  CHECK(session.phase() == cast::CastSession::Phase::Failed);
  CHECK(session.lastError().find("Soar Test Renderer") != std::string::npos);
  CHECK(session.lastError().find("has no AVTransport service") != std::string::npos);
  CHECK(bridge.playbackUrl().rfind("http://127.0.0.1:", 0) == 0);

  // An unnamed renderer is reported by its location instead of a blank name.
  // A failed session cannot restart (start() demands Idle), so a fresh one.
  config.empty_name = true;
  FakeRenderer unnamed(config);
  cast::CastSession session2;
  params.renderer_location = unnamed.location();
  CHECK_FALSE(session2.start(params));
  CHECK(session2.phase() == cast::CastSession::Phase::Failed);
  CHECK(session2.lastError().find(unnamed.location()) != std::string::npos);
  CHECK(session2.lastError().find("has no AVTransport service") != std::string::npos);
}

TEST_CASE("a rejected Play fails the session with the fault text") {
  const SoloStore store = makeSolo("faultplay");
  p2p::TorrentStream bridge;
  p2p::TorrentStream::Params bp;
  bp.torrent_path = store.torrent;
  bp.store_dir = store.dir;
  REQUIRE(bridge.start(bp));

  FakeRenderer::Config config;
  config.fault_play = true;
  FakeRenderer renderer(config);

  cast::CastSession session;
  cast::CastSession::Params params;
  params.bridge = &bridge;
  params.renderer_location = renderer.location();
  CHECK_FALSE(session.start(params));
  CHECK(session.lastError().find("cast: Play rejected: SOAP fault 716") !=
        std::string::npos);
  CHECK(bridge.playbackUrl().rfind("http://127.0.0.1:", 0) == 0);
}

TEST_CASE("a description that is not HTTP or declares a giant length fails clean") {
  const SoloStore store = makeSolo("baddesc");
  p2p::TorrentStream bridge;
  p2p::TorrentStream::Params bp;
  bp.torrent_path = store.torrent;
  bp.store_dir = store.dir;
  REQUIRE(bridge.start(bp));

  FakeRenderer::Config config;
  config.not_http = true;
  {
    FakeRenderer renderer(config);
    cast::CastSession session;
    cast::CastSession::Params params;
    params.bridge = &bridge;
    params.renderer_location = renderer.location();
    CHECK_FALSE(session.start(params));
    CHECK(session.lastError().find("not an HTTP reply") != std::string::npos);
  }
  config = FakeRenderer::Config{};
  config.huge_desc_length = true;
  {
    FakeRenderer renderer(config);
    cast::CastSession session;
    cast::CastSession::Params params;
    params.bridge = &bridge;
    params.renderer_location = renderer.location();
    CHECK_FALSE(session.start(params));
    CHECK(session.lastError().find("reply too large") != std::string::npos);
  }
  CHECK(bridge.playbackUrl().rfind("http://127.0.0.1:", 0) == 0);
}

TEST_CASE("a discovery window with no renderer answers fails explicitly") {
  const SoloStore store = makeSolo("nodisc");
  p2p::TorrentStream bridge;
  p2p::TorrentStream::Params bp;
  bp.torrent_path = store.torrent;
  bp.store_dir = store.dir;
  REQUIRE(bridge.start(bp));

  cast::CastSession session;
  cast::CastSession::Params params;
  params.bridge = &bridge;
  params.renderer_location = "";  // discovery it is
  params.discover_ms = 300;
  CHECK_FALSE(session.start(params));
  CHECK(session.lastError() ==
        "cast: no DLNA renderer answered the discovery window");
}

TEST_CASE("a cast session drives the renderer and restores the bridge") {
  const SoloStore store = makeSolo("drive");
  p2p::TorrentStream bridge;
  p2p::TorrentStream::Params bp;
  bp.torrent_path = store.torrent;
  bp.store_dir = store.dir;
  REQUIRE(bridge.start(bp));
  REQUIRE(bridge.phase() == p2p::TorrentPhase::Serving);
  const std::string loopback_url = bridge.playbackUrl();
  CHECK(loopback_url.rfind("http://127.0.0.1:", 0) == 0);

  FakeRenderer::Config config;
  FakeRenderer renderer(config);

  TickCollector ticks;
  cast::CastSession session;
  cast::CastSession::Params params;
  params.bridge = &bridge;
  params.renderer_location = renderer.location();
  params.title = "Cast Me <Now>";
  params.poll_ms = 120;
  params.start_grace_ms = 20000;
  params.on_position = std::ref(ticks);
  REQUIRE_MESSAGE(session.start(params), "start failed: " << session.lastError());
  CHECK(session.phase() == cast::CastSession::Phase::Casting);
  CHECK(session.rendererName() == "Soar Test Renderer");
  // A second start is refused without disturbing the running session.
  CHECK_FALSE(session.start(params));
  CHECK(session.lastError() == "cast: already started");
  CHECK(session.phase() == cast::CastSession::Phase::Casting);

  // The renderer was handed the post-expose URL and could fetch it: the
  // bridge served the cast source through the LAN window (206 = Range).
  const std::string cast_url = renderer.lastUri();
  CHECK(cast_url == bridge.playbackUrl());
  CHECK(cast_url.rfind("http://127.0.0.1:", 0) == 0);  // the test host's LAN address IS loopback
  CHECK(renderer.mediaFetchStatus() == 206);
  CHECK(renderer.lastTitle() == "Cast Me <Now>");
  CHECK(renderer.sawPlay());
  // The event stream subscribed alongside the control plane and retires
  // with the session.
  CHECK(renderer.sawSubscribe());

  // Position ticks advance at the renderer.
  CHECK(waitTicks(ticks, 4, 8000));
  if (!ticks.strictlyIncreasingTail()) {
    std::lock_guard<std::mutex> lock(ticks.mu);
    for (const auto& t : ticks.ticks) {
      MESSAGE("tick pos=" << t.first << " dur=" << t.second);
    }
  }
  CHECK(ticks.strictlyIncreasingTail());

  // A seek reaches the renderer as a REL_TIME target.
  REQUIRE(session.seekMs(5000));
  bool seek_seen = false;
  for (int i = 0; i < 100 && !seek_seen; ++i) {
    seek_seen = renderer.lastSeekTarget() == "0:00:05";
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK(seek_seen);

  // Stop tells the renderer and puts the bridge back on loopback.
  session.stop();
  CHECK(session.phase() == cast::CastSession::Phase::Stopped);
  CHECK(renderer.sawStop());
  CHECK(renderer.sawUnsubscribed());
  CHECK(bridge.phase() == p2p::TorrentPhase::Serving);  // the bridge lives on
  CHECK(bridge.playbackUrl().rfind("http://127.0.0.1:", 0) == 0);
  CHECK(getStatus(bridge.playbackUrl()) == 206);
}

TEST_CASE("pause reaches the renderer, holds the clock, and resumes from it") {
  const SoloStore store = makeSolo("pause");
  p2p::TorrentStream bridge;
  p2p::TorrentStream::Params bp;
  bp.torrent_path = store.torrent;
  bp.store_dir = store.dir;
  REQUIRE(bridge.start(bp));

  FakeRenderer::Config config;
  FakeRenderer renderer(config);

  TickCollector ticks;
  cast::CastSession session;
  cast::CastSession::Params params;
  params.bridge = &bridge;
  params.renderer_location = renderer.location();
  params.poll_ms = 120;
  params.start_grace_ms = 30000;
  params.on_position = std::ref(ticks);
  REQUIRE_MESSAGE(session.start(params), "start failed: " << session.lastError());

  // Let the clock move before pausing: the renderer's held value is then a
  // real number, so the "still advancing after resume" assertion has
  // something to compare against.
  CHECK(waitTicks(ticks, 2, 8000));

  REQUIRE(session.pause());
  CHECK(session.paused());
  CHECK(session.pause());  // idempotent: a second pause is a no-op
  bool pause_seen = false;
  for (int i = 0; i < 100 && !pause_seen; ++i) {
    pause_seen = renderer.sawPause();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK(pause_seen);
  // Drain the poll that may have been in flight when Pause landed: its
  // answer still carries the pre-pause clock.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  // The renderer's clock holds and the session stays casting.
  const std::int64_t held = ticks.lastPos();
  CHECK(held > 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  CHECK(ticks.lastPos() == held);
  CHECK(session.phase() == cast::CastSession::Phase::Casting);
  CHECK(renderer.clockHeld());

  REQUIRE(session.resume());
  CHECK_FALSE(session.paused());
  CHECK(session.resume());  // idempotent: resuming an unpaused session is a no-op
  // waitTicks counts absolute size, and the vector already holds plenty of
  // pre-pause ticks — wait for two NEW ones instead.
  const size_t before = ticks.size();
  REQUIRE(waitTicks(ticks, before + 2, 8000));
  CHECK(ticks.lastPos() > held);  // the clock ran on from the freeze point

  session.stop();
  CHECK_FALSE(session.paused());  // the flag does not survive the stop
}

TEST_CASE("a rejected Pause leaves the session casting and unpaused") {
  const SoloStore store = makeSolo("pausefault");
  p2p::TorrentStream bridge;
  p2p::TorrentStream::Params bp;
  bp.torrent_path = store.torrent;
  bp.store_dir = store.dir;
  REQUIRE(bridge.start(bp));

  FakeRenderer::Config config;
  config.fault_pause = true;
  FakeRenderer renderer(config);

  cast::CastSession session;
  cast::CastSession::Params params;
  params.bridge = &bridge;
  params.renderer_location = renderer.location();
  params.poll_ms = 120;
  params.start_grace_ms = 30000;
  REQUIRE_MESSAGE(session.start(params), "start failed: " << session.lastError());
  CHECK_FALSE(session.pause());
  CHECK(session.paused() == false);  // the session did not claim to hold
  CHECK(session.lastError().find("cast: Pause rejected: SOAP fault") !=
        std::string::npos);
  // Seeking still works after a refused Pause.
  CHECK(session.seekMs(3000));
  session.stop();
  CHECK(session.phase() == cast::CastSession::Phase::Stopped);
}

TEST_CASE("a PLAYING event over GENA keeps a stalled renderer past the grace window") {
  const SoloStore store = makeSolo("gena");
  p2p::TorrentStream bridge;
  p2p::TorrentStream::Params bp;
  bp.torrent_path = store.torrent;
  bp.store_dir = store.dir;
  REQUIRE(bridge.start(bp));

  FakeRenderer::Config config;
  config.stalled = true;  // GetPositionInfo answers 0:00:00 forever
  FakeRenderer renderer(config);

  std::mutex tmu;
  std::vector<cast::TransportState> seen;
  cast::CastSession session;
  cast::CastSession::Params params;
  params.bridge = &bridge;
  params.renderer_location = renderer.location();
  params.poll_ms = 120;
  params.start_grace_ms = 1500;  // without an event this session is Failed now
  params.on_transport = [&](cast::TransportState s) {
    std::lock_guard<std::mutex> lock(tmu);
    seen.push_back(s);
  };
  REQUIRE_MESSAGE(session.start(params), "start failed: " << session.lastError());
  CHECK(renderer.sawSubscribe());

  // The renderer announces PLAYING over the callback: evidence of life
  // the poll clock cannot show.
  CHECK(renderer.pushNotify(notifyBody("PLAYING")) == 200);
  bool playing_seen = false;
  for (int i = 0; i < 100 && !playing_seen; ++i) {
    std::lock_guard<std::mutex> lock(tmu);
    playing_seen = !seen.empty() && seen.back() == cast::TransportState::Playing;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK(playing_seen);

  // Well past the grace window the session is still casting — the event
  // suppressed the decode-failure verdict the stalled poll would earn.
  std::this_thread::sleep_for(std::chrono::milliseconds(2500));
  CHECK(session.phase() == cast::CastSession::Phase::Casting);

  session.stop();
  CHECK(renderer.sawUnsubscribed());
}

TEST_CASE("a refused subscription leaves the session poll-only and playing") {
  const SoloStore store = makeSolo("nosub");
  p2p::TorrentStream bridge;
  p2p::TorrentStream::Params bp;
  bp.torrent_path = store.torrent;
  bp.store_dir = store.dir;
  REQUIRE(bridge.start(bp));

  FakeRenderer::Config config;
  config.refuse_subscribe = true;
  FakeRenderer renderer(config);

  TickCollector ticks;
  cast::CastSession session;
  cast::CastSession::Params params;
  params.bridge = &bridge;
  params.renderer_location = renderer.location();
  params.poll_ms = 120;
  params.start_grace_ms = 30000;
  params.on_position = std::ref(ticks);
  REQUIRE_MESSAGE(session.start(params), "start failed: " << session.lastError());
  CHECK(session.phase() == cast::CastSession::Phase::Casting);
  // The renderer answered 412; the session subscribed once, took the
  // refusal, and went on polling.
  CHECK(renderer.sawSubscribe());
  CHECK(waitTicks(ticks, 2, 8000));

  session.stop();
  CHECK(session.phase() == cast::CastSession::Phase::Stopped);
  CHECK_FALSE(renderer.sawUnsubscribed());  // nothing to tear down
}

TEST_CASE("a short event lease is renewed while the session runs") {
  const SoloStore store = makeSolo("renew");
  p2p::TorrentStream bridge;
  p2p::TorrentStream::Params bp;
  bp.torrent_path = store.torrent;
  bp.store_dir = store.dir;
  REQUIRE(bridge.start(bp));

  FakeRenderer::Config config;
  config.short_event_lease = true;  // Second-5: due again within the test
  FakeRenderer renderer(config);

  cast::CastSession session;
  cast::CastSession::Params params;
  params.bridge = &bridge;
  params.renderer_location = renderer.location();
  params.poll_ms = 120;
  params.start_grace_ms = 30000;
  REQUIRE_MESSAGE(session.start(params), "start failed: " << session.lastError());
  bool renewed = false;
  for (int i = 0; i < 100 && !renewed; ++i) {
    renewed = renderer.renewCount() >= 1;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK(renewed);
  CHECK(session.phase() == cast::CastSession::Phase::Casting);

  session.stop();
  CHECK(renderer.sawUnsubscribed());
}

TEST_CASE("a renderer fault on SetAVTransportURI fails the session with the fault text") {
  const SoloStore store = makeSolo("fault");
  p2p::TorrentStream bridge;
  p2p::TorrentStream::Params bp;
  bp.torrent_path = store.torrent;
  bp.store_dir = store.dir;
  REQUIRE(bridge.start(bp));

  FakeRenderer::Config config;
  config.fault_set_uri = true;
  FakeRenderer renderer(config);

  cast::CastSession session;
  cast::CastSession::Params params;
  params.bridge = &bridge;
  params.renderer_location = renderer.location();
  CHECK_FALSE(session.start(params));
  CHECK(session.phase() == cast::CastSession::Phase::Failed);
  REQUIRE_MESSAGE(session.lastError().find("SOAP fault 716") != std::string::npos,
                  "unexpected error: " << session.lastError());
  CHECK(session.lastError().find("Resource not found") != std::string::npos);
  // The bridge is back on loopback after the failed start.
  CHECK(bridge.playbackUrl().rfind("http://127.0.0.1:", 0) == 0);
  CHECK(getStatus(bridge.playbackUrl()) == 206);
}

TEST_CASE("a renderer that never advances the position reports the decode failure") {
  const SoloStore store = makeSolo("stall");
  p2p::TorrentStream bridge;
  p2p::TorrentStream::Params bp;
  bp.torrent_path = store.torrent;
  bp.store_dir = store.dir;
  REQUIRE(bridge.start(bp));

  FakeRenderer::Config config;
  config.stalled = true;
  FakeRenderer renderer(config);

  cast::CastSession session;
  cast::CastSession::Params params;
  params.bridge = &bridge;
  params.renderer_location = renderer.location();
  params.poll_ms = 100;
  params.start_grace_ms = 800;
  REQUIRE_MESSAGE(session.start(params), "start failed: " << session.lastError());
  CHECK(waitPhase(session, cast::CastSession::Phase::Failed, 15000));
  CHECK(session.lastError().find("cannot decode") != std::string::npos);
  CHECK(session.lastError().find("transcoding was not requested") !=
        std::string::npos);
  // Failure cleanup restored the bridge.
  CHECK(bridge.playbackUrl().rfind("http://127.0.0.1:", 0) == 0);
}

TEST_CASE("a renderer that stops answering fails the session explicitly") {
  const SoloStore store = makeSolo("deaf");
  p2p::TorrentStream bridge;
  p2p::TorrentStream::Params bp;
  bp.torrent_path = store.torrent;
  bp.store_dir = store.dir;
  REQUIRE(bridge.start(bp));

  FakeRenderer::Config config;
  config.go_deaf_after = 2;
  FakeRenderer renderer(config);

  cast::CastSession session;
  cast::CastSession::Params params;
  params.bridge = &bridge;
  params.renderer_location = renderer.location();
  params.poll_ms = 100;
  params.start_grace_ms = 30000;  // not the failure under test here
  params.max_poll_failures = 3;
  REQUIRE_MESSAGE(session.start(params), "start failed: " << session.lastError());
  CHECK(waitPhase(session, cast::CastSession::Phase::Failed, 15000));
  CHECK(session.lastError().find("stopped answering") != std::string::npos);
  CHECK(bridge.playbackUrl().rfind("http://127.0.0.1:", 0) == 0);
}

#if !defined(__APPLE__)  // live multicast: see the note above
TEST_CASE("a session can start from discovery alone") {
  const SoloStore store = makeSolo("discoverstart");
  p2p::TorrentStream bridge;
  p2p::TorrentStream::Params bp;
  bp.torrent_path = store.torrent;
  bp.store_dir = store.dir;
  REQUIRE(bridge.start(bp));

  FakeRenderer::Config config;
  config.join_group = true;
  FakeRenderer renderer(config);

  cast::CastSession session;
  cast::CastSession::Params params;
  params.bridge = &bridge;
  params.renderer_location = "";  // discovery path
  params.discover_ms = 2500;
  REQUIRE_MESSAGE(session.start(params), "start failed: " << session.lastError());
  CHECK(session.phase() == cast::CastSession::Phase::Casting);
  CHECK(session.rendererName() == "Soar Test Renderer");
  CHECK(renderer.lastUri() == bridge.playbackUrl());
  session.stop();
  CHECK(session.phase() == cast::CastSession::Phase::Stopped);
  CHECK(renderer.sawStop());
}
#endif  // !defined(__APPLE__)

TEST_CASE("a Seek the renderer rejects is reported on the session") {
  const SoloStore store = makeSolo("faultseek");
  p2p::TorrentStream bridge;
  p2p::TorrentStream::Params bp;
  bp.torrent_path = store.torrent;
  bp.store_dir = store.dir;
  REQUIRE(bridge.start(bp));

  FakeRenderer::Config config;
  config.fault_seek = true;
  FakeRenderer renderer(config);

  cast::CastSession session;
  cast::CastSession::Params params;
  params.bridge = &bridge;
  params.renderer_location = renderer.location();
  REQUIRE_MESSAGE(session.start(params), "start failed: " << session.lastError());
  CHECK_FALSE(session.seekMs(1000));
  CHECK(session.lastError().find("cast: Seek rejected: SOAP fault 716") !=
        std::string::npos);
  CHECK(session.phase() == cast::CastSession::Phase::Casting);  // seek is not fatal
  session.stop();
}

TEST_CASE("a control URL scheme the control point rejects fails the session") {
  const SoloStore store = makeSolo("badcontrol");
  p2p::TorrentStream bridge;
  p2p::TorrentStream::Params bp;
  bp.torrent_path = store.torrent;
  bp.store_dir = store.dir;
  REQUIRE(bridge.start(bp));

  FakeRenderer::Config config;
  config.bad_control = true;
  FakeRenderer renderer(config);

  cast::CastSession session;
  cast::CastSession::Params params;
  params.bridge = &bridge;
  params.renderer_location = renderer.location();
  CHECK_FALSE(session.start(params));
  CHECK(session.lastError().find("cast: bad control URL ftp://") !=
        std::string::npos);
  CHECK(bridge.playbackUrl().rfind("http://127.0.0.1:", 0) == 0);
}

TEST_CASE("a clock-less renderer is never judged stuck") {
  const SoloStore store = makeSolo("noclock");
  p2p::TorrentStream bridge;
  p2p::TorrentStream::Params bp;
  bp.torrent_path = store.torrent;
  bp.store_dir = store.dir;
  REQUIRE(bridge.start(bp));

  FakeRenderer::Config config;
  config.no_clock = true;
  FakeRenderer renderer(config);

  TickCollector ticks;
  cast::CastSession session;
  cast::CastSession::Params params;
  params.bridge = &bridge;
  params.renderer_location = renderer.location();
  params.poll_ms = 80;
  params.start_grace_ms = 300;  // would fail a renderer stuck at 0:00
  params.on_position = std::ref(ticks);
  REQUIRE_MESSAGE(session.start(params), "start failed: " << session.lastError());
  // Far past the grace window the session still casts: NOT_IMPLEMENTED
  // means "unknown", not "stuck".
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  CHECK(session.phase() == cast::CastSession::Phase::Casting);
  CHECK(waitTicks(ticks, 3, 1000));
  {
    std::lock_guard<std::mutex> lock(ticks.mu);
    REQUIRE(ticks.ticks.size() >= 3);
    CHECK(ticks.ticks.back().first == -1);  // position unknown, verbatim
    CHECK(ticks.ticks.back().second == 60000);  // duration is known: 0:01:00
  }
  session.stop();
  CHECK(session.phase() == cast::CastSession::Phase::Stopped);
}

TEST_CASE("a fault without a description still names the code") {
  const SoloStore store = makeSolo("faultnodesc");
  p2p::TorrentStream bridge;
  p2p::TorrentStream::Params bp;
  bp.torrent_path = store.torrent;
  bp.store_dir = store.dir;
  REQUIRE(bridge.start(bp));

  FakeRenderer::Config config;
  config.fault_set_uri = true;
  config.fault_no_desc = true;
  FakeRenderer renderer(config);

  cast::CastSession session;
  cast::CastSession::Params params;
  params.bridge = &bridge;
  params.renderer_location = renderer.location();
  CHECK_FALSE(session.start(params));
  CHECK(session.lastError().find("SOAP fault 401") != std::string::npos);
  // No description followed the code.
  CHECK(session.lastError().find("SOAP fault 401: ") == std::string::npos);
}

TEST_CASE("description transport edges fail or recover with clear errors") {
  // A too-short declared length: the client trims the spill, and the
  // trimmed text is no longer a description.
  FakeRenderer::Config config;
  config.spill = true;
  {
    FakeRenderer renderer(config);
    cast::DeviceDesc desc;
    std::string err;
    CHECK_FALSE(cast::fetchDescription(renderer.location(), &desc, &err));
    CHECK(err.find("not a device description") != std::string::npos);
  }
  // No Content-Length: the body is read to the connection close and the
  // description parses.
  config = FakeRenderer::Config{};
  config.no_length = true;
  {
    FakeRenderer renderer(config);
    cast::DeviceDesc desc;
    std::string err;
    CHECK(cast::fetchDescription(renderer.location(), &desc, &err));
    CHECK(desc.friendly_name == "Soar Test Renderer");
  }
  // A giant unterminated head: the wire budget cuts the exchange.
  config = FakeRenderer::Config{};
  config.flood_head = true;
  {
    FakeRenderer renderer(config);
    cast::DeviceDesc desc;
    std::string err;
    CHECK_FALSE(cast::fetchDescription(renderer.location(), &desc, &err));
    CHECK(err.find("reply read failed") != std::string::npos);
  }
  // An unresolvable description host fails at name resolution.
  cast::DeviceDesc desc;
  std::string err;
  CHECK_FALSE(cast::fetchDescription("http://bad host/d.xml", &desc, &err));
  CHECK(err.find("cannot resolve") != std::string::npos);
}

TEST_CASE("pollPosition against a non-action endpoint reports the HTTP status") {
  FakeRenderer::Config config;
  FakeRenderer renderer(config);

  // A valid http URL whose path is neither the description nor a control
  // endpoint: the fake answers 404 and the client surfaces the status.
  std::string dead = renderer.location();
  dead.replace(dead.find("/desc.xml"), 9, "/elsewhere");
  cast::RendererClient client(dead, cast::kAvTransportType);
  cast::PositionInfo info;
  std::string err;
  CHECK_FALSE(client.pollPosition(&info, &err));
  CHECK(err == "renderer answered HTTP 404");
}

TEST_CASE("a session without a serving bridge or renderer fails fast") {
  p2p::TorrentStream bridge;
  FakeRenderer::Config no_script;
  FakeRenderer renderer(no_script);

  cast::CastSession session;
  cast::CastSession::Params params;
  session.stop();  // stopping an idle session is a no-op
  CHECK(session.phase() == cast::CastSession::Phase::Idle);
  params.bridge = nullptr;
  CHECK_FALSE(session.start(params));
  CHECK(session.lastError() == "cast: no bridge given");
  CHECK(session.phase() == cast::CastSession::Phase::Idle);

  params.bridge = &bridge;  // not started: not serving
  CHECK_FALSE(session.start(params));
  CHECK(session.lastError() == "cast: the bridge is not serving");
  CHECK_FALSE(session.seekMs(1000));  // no cast, no seek

  // An unreachable description URL fails with the transport error.
  p2p::TorrentStream::Params bp;
  const SoloStore store = makeSolo("deadrenderer");
  bp.torrent_path = store.torrent;
  bp.store_dir = store.dir;
  REQUIRE(bridge.start(bp));
  params.renderer_location = "http://127.0.0.1:1/desc.xml";
  CHECK_FALSE(session.start(params));
  CHECK(session.phase() == cast::CastSession::Phase::Failed);
  CHECK_FALSE(session.lastError().empty());
  CHECK(bridge.playbackUrl().rfind("http://127.0.0.1:", 0) == 0);

  // stop() is idempotent from every phase.
  session.stop();
  session.stop();
}

// ---- the GENA event server over real sockets ----------------------------
// The renderer POSTs NOTIFY requests here; these cases drive the accepted
// connections raw: well-formed bodies reach the callback, malformed and
// hostile clients get a bare 200 (or nothing) and never disturb the loop.

int dial(int port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(fd >= 0);
  timeval tv{};
  tv.tv_sec = 20;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<unsigned short>(port));
  REQUIRE(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);
  REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
  return fd;
}

// Sends everything, then reads until the server closes. "" on a reset.
std::string talk(int fd, const std::string& out) {
  size_t off = 0;
  while (off < out.size()) {
    const ssize_t w = ::send(fd, out.data() + off, out.size() - off, 0);
    REQUIRE(w > 0);
    off += static_cast<size_t>(w);
  }
  std::string all;
  char buf[4096];
  for (;;) {
    const ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
    if (r <= 0) break;
    all.append(buf, static_cast<size_t>(r));
  }
  return all;
}

bool replyIsBare200(const std::string& reply) {
  return reply.rfind("HTTP/1.1 200 OK\r\n", 0) == 0 &&
         reply.find("CONTENT-LENGTH: 0") != std::string::npos;
}

TEST_CASE("event server delivers NOTIFY bodies and shrugs off junk") {
  cast::EventServer es;
  std::string err;
  std::vector<std::string> bodies;
  REQUIRE(es.start([&bodies](std::string_view body) {
            bodies.emplace_back(body);
          }, &err));
  const int port = es.port();
  REQUIRE(port > 0);
  CHECK(es.callbackUrlFor("127.0.0.1") ==
        "http://127.0.0.1:" + std::to_string(port) + "/cast/events");

  // A well-formed NOTIFY: the callback fires with exactly the body.
  {
    const int fd = dial(port);
    const std::string body = "<e xmlns=\"urn:u\">LastChange blah</e>";
    const std::string reply =
        talk(fd, "NOTIFY /cast/events HTTP/1.1\r\nHOST: x\r\nCONTENT-TYPE: "
                 "text/xml\r\nCONTENT-LENGTH: " +
                     std::to_string(body.size()) + "\r\n\r\n" + body);
    CHECK(replyIsBare200(reply));
    REQUIRE(bodies.size() == 1);
    CHECK(bodies.back() == body);
    ::close(fd);
  }

  // A body inline after the blank line with no CONTENT-LENGTH at all:
  // whatever arrived is what the callback sees (renderers forget the
  // header; the poll path covers any loss).
  {
    const int fd = dial(port);
    const std::string reply =
        talk(fd, "NOTIFY /cast/events HTTP/1.1\r\nHOST: x\r\n\r\ninline-body");
    CHECK(replyIsBare200(reply));
    REQUIRE(bodies.size() == 2);
    CHECK(bodies.back() == "inline-body");
    ::close(fd);
  }

  // A CONTENT-LENGTH larger than what rode in with the head: the rest of
  // the body is read off the wire before the callback fires.
  {
    const int fd = dial(port);
    const std::string head =
        "NOTIFY /cast/events HTTP/1.1\r\nHOST: x\r\nCONTENT-LENGTH: 12\r\n\r\nabc";
    const std::string reply = talk(fd, head + "defghijklmno");
    CHECK(replyIsBare200(reply));
    REQUIRE(bodies.size() == 3);
    CHECK(bodies.back() == "abcdefghijklmno");
    ::close(fd);
  }

  // A complete head with no body: no callback, still a bare 200.
  {
    const int fd = dial(port);
    CHECK(replyIsBare200(talk(fd, "BLAH\r\n\r\n")));
    CHECK(bodies.size() == 3);
    ::close(fd);
  }

  // Connect and close without sending anything: the read fails, the loop
  // moves on to the next client.
  {
    const int fd = dial(port);
    ::close(fd);
    const int fd2 = dial(port);  // the accept loop still serves
    CHECK(replyIsBare200(talk(fd2, "BLAH\r\n\r\n")));
    ::close(fd2);
  }

  es.stop();
  CHECK(es.port() == 0);
  CHECK(es.callbackUrlFor("127.0.0.1").empty());
}

TEST_CASE("an oversized headless request is refused, not fatal") {
  // Regression: a flood past the size cap without a blank line used to
  // reach head.resize(npos) — length_error out of serveConnection and the
  // whole process down. The reply must go out and the server must live.
  cast::EventServer es;
  std::string err;
  int fired = 0;
  REQUIRE(es.start([&fired](std::string_view) { ++fired; }, &err));
  const int port = es.port();

  {
    const int fd = dial(port);
    // Bigger than kMaxNotify (64 KiB), never terminated.
    CHECK(replyIsBare200(talk(fd, std::string(70 * 1024, 'A'))));
    ::close(fd);
  }

  // The loop survived; a normal NOTIFY still lands.
  {
    const int fd = dial(port);
    const std::string reply = talk(fd, "NOTIFY / HTTP/1.1\r\n\r\nafter-flood");
    CHECK(replyIsBare200(reply));
    CHECK(fired == 1);
    ::close(fd);
  }
  es.stop();
}

TEST_CASE("event server rejects degenerate calls cleanly") {
  cast::EventServer es;
  std::string err;
  es.stop();  // stopping an idle server is a no-op

  // No callback: connections are answered, nothing fires.
  REQUIRE(es.start(nullptr, &err));
  const int port = es.port();
  REQUIRE(port > 0);
  {
    const int fd = dial(port);
    CHECK(replyIsBare200(
        talk(fd, "NOTIFY / HTTP/1.1\r\nCONTENT-LENGTH: 5\r\n\r\nhello")));
    ::close(fd);
  }

  // A client that sends nothing gets the bare 200 after the read timeout
  // and the loop keeps serving (same server, next case runs fresh).
  CHECK(es.start(nullptr, &err));  // starting twice is a no-op, same port
  CHECK(es.port() == port);

  es.stop();
  // The port can be bound again right away.
  REQUIRE(es.start(nullptr, &err));
  CHECK(es.port() > 0);
  // A named peer resolves through getaddrinfo ("localhost" is never a
  // dotted quad) and still advertises a local callback URL.
  CHECK(es.callbackUrlFor("localhost").rfind("http://", 0) == 0);
  es.stop();
}

TEST_CASE("a silent event client hits the head timeout and still gets the 200") {
  cast::EventServer es;
  std::string err;
  REQUIRE(es.start(nullptr, &err));
  const int fd = dial(es.port());
  const auto t0 = std::chrono::steady_clock::now();
  // Nothing is ever sent; the reply only comes after the server's read
  // timeout (5 s), so a fast reply here would mean the timeout arm died.
  CHECK(replyIsBare200(talk(fd, "")));
  CHECK(std::chrono::steady_clock::now() - t0 > std::chrono::seconds(4));
  ::close(fd);
  es.stop();
}

// ---- sender-side transcoding (src/cast/transcode_server.cpp) -----------
// The pump runs against the same tiny committed fixtures the player suite
// uses: one H.264/AAC source the MPEG-TS pair accepts (copied through),
// one MPEG-4/MP3 source (decoded and re-encoded). Both end to end: the
// pump demuxes a real (bridge) URL, the HTTP endpoint streams to a real
// client, and a cast session hands the whole thing to the fixture
// renderer.

#ifdef SOAR_WITH_FFMPEG

std::string readFixture(const char* rel) {
  std::ifstream f(std::string(SOAR_TEST_CAST_FIXTURES) + "/" + rel,
                  std::ios::binary);
  REQUIRE(f.good());
  std::string out((std::istreambuf_iterator<char>(f)),
                  std::istreambuf_iterator<char>());
  REQUIRE(!out.empty());
  return out;
}

// A committed media fixture by name, at the compile-time-pinned absolute
// path (the CWD of a ctest process is the build tree, so relative names
// would resolve against it and miss).
std::string castFixture(const char* name) {
  return std::string(SOAR_TEST_CAST_FIXTURES) + "/" + name;
}

// GETs the TS endpoint and reads the head plus the body until the server
// closes (a finite source ends its stream — no Range anywhere: the
// endpoint has one live resource).
struct StreamFetch {
  int status = 0;
  std::string content_type;
  std::string body;
};

StreamFetch fetchStream(const std::string& host, int port) {
  StreamFetch out;
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(fd >= 0);
  timeval tv{};
  tv.tv_sec = 20;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<unsigned short>(port));
  REQUIRE(::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) == 1);
  REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
  const std::string req =
      "GET / HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
  size_t off = 0;
  while (off < req.size()) {
    const ssize_t w = ::send(fd, req.data() + off, req.size() - off, 0);
    REQUIRE(w > 0);
    off += static_cast<size_t>(w);
  }
  std::string all;
  char buf[16384];
  for (;;) {
    const ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
    if (r <= 0) break;
    all.append(buf, static_cast<size_t>(r));
  }
  ::close(fd);
  const size_t head_end = all.find("\r\n\r\n");
  REQUIRE(head_end != std::string::npos);
  const std::string head = all.substr(0, head_end);
  const char* sp = std::strchr(head.c_str(), ' ');
  out.status = sp != nullptr ? std::atoi(sp + 1) : 0;
  for (size_t i = 0; i + 1 < head.size(); ++i) {
    if ((head[i] == 'c' || head[i] == 'C') &&
        cast::ciEqual(head.substr(i, 13), "content-type:")) {
      size_t b = i + 13;
      while (b < head.size() && head[b] == ' ') ++b;
      size_t e = head.find("\r\n", b);
      if (e == std::string::npos) e = head.size();
      out.content_type = head.substr(b, e - b);
      break;
    }
  }
  out.body = all.substr(head_end + 4);
  return out;
}

bool tsSync(const std::string& body) {
  // MPEG-TS: one 0x47 sync byte per 188-byte packet. A few hundred
  // packets of pacing is far beyond coincidence.
  const size_t packets = std::min(body.size() / 188, size_t{200});
  if (packets < 10) return false;
  for (size_t i = 0; i < packets; ++i) {
    if (static_cast<unsigned char>(body[i * 188]) != 0x47) return false;
  }
  return true;
}

// The stream codecs ffprobe finds in a fetched TS body ("h264\naac\n"
// shape), proving both streams really travel — a byte count alone once
// hid an audio path that produced nothing. "" when ffprobe is absent.
std::string probeCodecs(const std::string& body) {
  if (std::system("command -v ffprobe >/dev/null 2>&1") != 0) return "";
  const std::string dir = makeTempDir("cast-tsprobe");
  const std::string file = dir + "/out.ts";
  writeBytes(file, body);
  const std::string cmd = "ffprobe -v error -show_entries stream=codec_name "
                          "-of csv=p=0 " + file + " 2>/dev/null";
  std::string codecs;
  if (FILE* probe = ::popen(cmd.c_str(), "r")) {
    char line[64];
    while (::fgets(line, sizeof(line), probe) != nullptr) codecs += line;
    ::pclose(probe);
  }
  (void)std::system(("rm -rf " + dir).c_str());
  return codecs;
}

// A WAV of sawtooth PCM `seconds` long (44.1 kHz stereo 16-bit): the raw
// samples are valid noise audio, so the pump encodes it into MPEG-TS well
// past the 1 MiB in-flight queue — the one way to reach the queue-full
// backpressure state with a small test footprint.
std::string makeNoiseWav(const char* tag, int seconds) {
  const int rate = 44100, channels = 2, bytes_per_sample = 2;
  const std::string pcm = patternBytes(static_cast<size_t>(rate) * channels *
                                       bytes_per_sample *
                                       static_cast<size_t>(seconds));
  const auto le = [](unsigned v, int n) {
    std::string s;
    for (int i = 0; i < n; ++i) {
      s.push_back(static_cast<char>(v & 0xff));
      v >>= 8;
    }
    return s;
  };
  std::string h = "RIFF" + le(0, 4) + "WAVEfmt " + le(16, 4) + le(1, 2) +
                  le(channels, 2) + le(rate, 4) + le(rate * channels * 2, 4) +
                  le(channels * 2, 2) + le(16, 2) + "data" + le(pcm.size(), 4);
  h.replace(4, 4, le(static_cast<unsigned>(pcm.size()) + 36, 4));
  const std::string dir = makeTempDir(tag);
  writeBytes(dir + "/noise.wav", h + pcm);
  return dir + "/noise.wav";
}

std::string makeEmptyWav(const char* tag) {
  // A valid PCM header with a zero-length data chunk: the demuxer hands the
  // pump a real audio stream that yields no packets at all.
  const int rate = 44100, channels = 2;
  const auto le = [](unsigned v, int n) {
    std::string s;
    for (int i = 0; i < n; ++i) {
      s.push_back(static_cast<char>(v & 0xff));
      v >>= 8;
    }
    return s;
  };
  std::string h = "RIFF" + le(36, 4) + "WAVEfmt " + le(16, 4) + le(1, 2) +
                  le(channels, 2) + le(rate, 4) + le(rate * channels * 2, 4) +
                  le(channels * 2, 2) + le(16, 2) + "data" + le(0, 4);
  const std::string dir = makeTempDir(tag);
  writeBytes(dir + "/empty.wav", h);
  return dir + "/empty.wav";
}

TEST_CASE("transcode reports a source that opens but holds no media") {
  // A stream can be declared and still yield nothing: a zero-sample track
  // opens fine and reads EOF at once, and the pump must not finish as a
  // silent success with an empty error — that same shape arrives through a
  // demuxer that hands a timecode track over as a video stream.
  cast::TranscodeServer ts;
  std::string err;
  REQUIRE(ts.start(makeEmptyWav("tsempty"), &err));
  REQUIRE_MESSAGE(ts.waitReady(20000) == false, ts.lastError());
  // A failed text match is undiagnosable without the actual verdict.
  CHECK_MESSAGE(ts.lastError().find("no video or audio stream") != std::string::npos,
                ts.lastError());
  ts.stop();
}

TEST_CASE("transcode copies an already-compatible source to MPEG-TS") {
  cast::TranscodeServer ts;
  std::string err;
  REQUIRE(ts.start(castFixture("cast_h264_aac.mp4"), &err));
  REQUIRE(ts.waitReady(20000));

  const StreamFetch got = fetchStream("127.0.0.1", ts.port());
  CHECK(got.status == 200);
  CHECK(got.content_type == "video/mp2t");
  // Two seconds of 64k video + 24k audio plus TS overhead: tens of KB.
  CHECK(got.body.size() > 20000);
  CHECK(tsSync(got.body));
  // The copied pair must both be in there (mp4 AAC needs its ADTS
  // framing rebuilt for TS — the muxer does it from the extradata).
  const std::string codecs = probeCodecs(got.body);
  if (!codecs.empty()) {
    CHECK(codecs.find("h264") != std::string::npos);
    CHECK(codecs.find("aac") != std::string::npos);
  }

  ts.stop();
}

TEST_CASE("transcode re-encodes a source the pair does not accept") {
  cast::TranscodeServer ts;
  std::string err;
  REQUIRE(ts.start(castFixture("cast_mpeg4_mp3.mp4"), &err));
  // The pump's last error is the diagnostic a failed readiness needs.
  REQUIRE_MESSAGE(ts.waitReady(20000), ts.lastError());  // mpeg4+mp3: decode + x264/aac encode

  const StreamFetch got = fetchStream("127.0.0.1", ts.port());
  CHECK(got.status == 200);
  CHECK(got.body.size() > 20000);
  CHECK(tsSync(got.body));
  // Both re-encoded streams must survive the whole chain — this probe is
  // what catches a resample path that quietly produces no samples.
  const std::string codecs = probeCodecs(got.body);
  if (!codecs.empty()) {
    CHECK(codecs.find("h264") != std::string::npos);
    CHECK(codecs.find("aac") != std::string::npos);
  }

  ts.stop();
}

TEST_CASE("transcode reports a broken source through waitReady") {
  cast::TranscodeServer ts;
  std::string err;
  REQUIRE(ts.start(castFixture("no-such-file.mp4"), &err));
  CHECK_FALSE(ts.waitReady(20000));
  CHECK_FALSE(ts.lastError().empty());
  ts.stop();
}

TEST_CASE("transcode serves one stream at a time") {
  // Two things can free the slot while this test runs and turn the second
  // fetch into a 200 instead of a 503: the source running to EOF (so the
  // source is long), and the stalled-reader send timeout (so the first
  // client keeps reading). Both are real release paths, not test noise.
  cast::TranscodeServer ts;
  std::string err;
  REQUIRE(ts.start(makeNoiseWav("tsone", 90), &err));
  REQUIRE(ts.waitReady(20000));

  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(fd >= 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<unsigned short>(ts.port()));
  REQUIRE(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);
  REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
  const std::string req = "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
  REQUIRE(::send(fd, req.data(), req.size(), 0) > 0);

  std::atomic<bool> started{false};
  std::thread drain([&fd, &started] {
    char b[8192];
    std::string got;
    for (;;) {
      const ssize_t n = ::recv(fd, b, sizeof(b), 0);
      if (n <= 0) break;
      got.append(b, static_cast<size_t>(n));
      if (!started && got.find("HTTP/1.1 200 OK") != std::string::npos)
        started = true;
    }
  });
  for (int i = 0; i < 300 && !started; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  REQUIRE(started);

  // A second renderer-side fetch while the first holds the slot: 503.
  const StreamFetch second = fetchStream("127.0.0.1", ts.port());
  CHECK(second.status == 503);

  // The drain never closes the socket: stop() ends the stream, the server
  // side closes it, and the drain sees EOF and joins. main closing the
  // fd would race the drain's recv (and two closes of one fd would race
  // each other) — both are flagged by TSan.
  ts.stop();
  drain.join();
}

TEST_CASE("transcode scales an odd-size video-only source") {
  // 161x91 forces the even-size + scale branch, and with no audio stream
  // the muxer runs video-only.
  cast::TranscodeServer ts;
  std::string err;
  REQUIRE(ts.start(castFixture("cast_odd_video_only.mp4"), &err));
  REQUIRE(ts.waitReady(20000));

  const StreamFetch got = fetchStream("127.0.0.1", ts.port());
  CHECK(got.status == 200);
  CHECK(got.body.size() > 10000);
  CHECK(tsSync(got.body));

  ts.stop();
}

TEST_CASE("the transcode URL routes toward the renderer host") {
  cast::TranscodeServer ts;
  CHECK(ts.urlFor("127.0.0.1").empty());  // not started: nothing to route to

  std::string err;
  REQUIRE(ts.start(castFixture("cast_h264_aac.mp4"), &err));
  const std::string url = ts.urlFor("127.0.0.1");
  CHECK(url.rfind("http://127.0.0.1:", 0) == 0);

  ts.stop();
  CHECK(ts.urlFor("127.0.0.1").empty());  // stopped: the endpoint is gone
}

// The cast-session wiring over a real bridge: media.bin is the fixture's
// bytes, so the pump demuxes a real torrent-served URL.
TEST_CASE("a transcode session hands the renderer the TS endpoint") {
  SoloStore store;
  store.dir = makeTempDir("cast-ts");
  store.bytes = readFixture("cast_h264_aac.mp4");
  writeBytes(store.dir + "/media.bin", store.bytes);
  store.torrent = buildTorrent(store.dir, store.dir + "/media.torrent");

  p2p::TorrentStream bridge;
  p2p::TorrentStream::Params bp;
  bp.torrent_path = store.torrent;
  bp.store_dir = store.dir;
  REQUIRE(bridge.start(bp));
  REQUIRE(bridge.phase() == p2p::TorrentPhase::Serving);

  FakeRenderer fr({});
  REQUIRE(fr.location().rfind("http://127.0.0.1:", 0) == 0);

  cast::CastSession session;
  cast::CastSession::Params params;
  params.bridge = &bridge;
  params.renderer_location = fr.location();
  params.transcode = true;
  REQUIRE(session.start(params));
  REQUIRE(waitPhase(session, cast::CastSession::Phase::Casting, 30000));

  REQUIRE(fr.lastUri().rfind("http://", 0) == 0);
  // The renderer was pointed at the transcode endpoint, the DIDL
  // advertises the stream type, and the renderer's own fetch of that URI
  // succeeded (a working HTTP stream, not the bridge URL).
  CHECK(fr.lastUri() != bridge.playbackUrl());
  CHECK(fr.lastDidl().find("protocolInfo=\"http-get:*:video/mpeg:*\"") !=
        std::string::npos);
  CHECK(fr.mediaFetchStatus() == 200);

  // Seek has nothing to seek into on a live TS point.
  CHECK_FALSE(session.seekMs(5000));
  CHECK(session.lastError().find("transcoding") != std::string::npos);

  session.stop();
  CHECK(bridge.playbackUrl().rfind("http://127.0.0.1:", 0) == 0);
}

TEST_CASE("transcode refuses degenerate lifecycle calls cleanly") {
  cast::TranscodeServer ts;
  std::string err;
  ts.stop();  // stopping an idle server is a no-op
  CHECK_FALSE(ts.waitReady(50));  // nothing will ever be ready
  CHECK(ts.port() == 0);

  // Starting twice is a no-op: the second call keeps the first server.
  REQUIRE(ts.start(castFixture("cast_h264_aac.mp4"), &err));
  const int port = ts.port();
  REQUIRE(port > 0);
  CHECK(ts.start(castFixture("cast_mpeg4_mp3.mp4"), &err));
  CHECK(ts.port() == port);
  // The pump is still the first source's: it readies on the h264 copy.
  REQUIRE(ts.waitReady(20000));

  // The endpoint survives the redundant start and keeps serving.
  const StreamFetch got = fetchStream("127.0.0.1", port);
  CHECK(got.status == 200);
  ts.stop();
}

TEST_CASE("transcode drops silent, reset, and junk request heads") {
  cast::TranscodeServer ts;
  std::string err;
  REQUIRE(ts.start(castFixture("cast_h264_aac.mp4"), &err));
  REQUIRE(ts.waitReady(20000));
  const int port = ts.port();

  // Silence: the head read times out, the connection closes with no
  // reply, and the endpoint takes the next client.
  {
    const int fd = dial(port);
    CHECK(talk(fd, "").empty());
    ::close(fd);
  }

  // A partial head closed with a reset (SO_LINGER 0): the read fails and
  // the connection is dropped — no reply, no disturbance.
  {
    const int fd = dial(port);
    const std::string partial = "GET / HT";
    size_t off = 0;
    while (off < partial.size()) {
      const ssize_t w = ::send(fd, partial.data() + off, partial.size() - off, 0);
      REQUIRE(w > 0);
      off += static_cast<size_t>(w);
    }
    linger l{};
    l.l_onoff = 1;
    l.l_linger = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&l),
                 sizeof(l));
    ::close(fd);  // RST, not FIN
  }

  // Any head that completes is a GET: the endpoint has one resource.
  {
    const int fd = dial(port);
    const std::string got = talk(fd, "BLAH\r\n\r\n");
    CHECK(got.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
    ::close(fd);
  }

  // The slot is free again after all three. The junk-head client above
  // drained its whole (finite) stream before closing, and the flag clears
  // right after its final close — poll rather than race it.
  StreamFetch got;
  for (int i = 0; i < 25 && got.status != 200; ++i) {
    got = fetchStream("127.0.0.1", port);
    if (got.status != 200) std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  CHECK(got.status == 200);
  ts.stop();
}

TEST_CASE("transcode stop joins a client holding the stream open") {
  cast::TranscodeServer ts;
  std::string err;
  REQUIRE(ts.start(castFixture("cast_h264_aac.mp4"), &err));
  REQUIRE(ts.waitReady(20000));

  // Take the stream and stop reading: the serve thread blocks in send and
  // the pump backs up behind the queue. stop() must tear all of it down
  // and return instead of waiting on the client.
  const int port = ts.port();
  const int fd = dial(port);
  const std::string req = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
  REQUIRE(::send(fd, req.data(), req.size(), 0) == static_cast<ssize_t>(req.size()));
  char buf[512];
  const ssize_t head = ::recv(fd, buf, sizeof(buf), 0);
  REQUIRE(head > 0);  // the 200 head (and probably some stream bytes)

  ts.stop();  // joins pump, accept, and the mid-stream serve thread
  CHECK(ts.port() == 0);
  CHECK(ts.urlFor("127.0.0.1").empty());
  // The listener is gone: a new fetch cannot even connect.
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<unsigned short>(port));
  REQUIRE(::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1);
  const int fd2 = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(fd2 >= 0);
  CHECK(::connect(fd2, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0);
  ::close(fd2);
  ::close(fd);
}

TEST_CASE("a client that stops reading parks the pump until stop") {
  // The queue is the flow control: a source whose TS output outgrows the
  // 1 MiB in-flight cap with nobody draining it parks the pump in the
  // queue-full wait. stop() must break the wait and join everything.
  const std::string wav = makeNoiseWav("tsbp", 90);
  cast::TranscodeServer ts;
  std::string err;
  REQUIRE(ts.start(wav, &err));
  REQUIRE(ts.waitReady(20000));  // the muxer header is out; the rest queues

  const int fd = dial(ts.port());
  const std::string req = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
  REQUIRE(::send(fd, req.data(), req.size(), 0) == static_cast<ssize_t>(req.size()));
  char buf[256];
  REQUIRE(::recv(fd, buf, sizeof(buf), 0) > 0);  // the 200 head, then silence

  // Give the pump time to fill the queue behind the unreading client
  // (encode-bound: several seconds for a minute of audio under -O0).
  std::this_thread::sleep_for(std::chrono::seconds(6));
  ts.stop();
  CHECK(ts.port() == 0);
  CHECK(ts.urlFor("127.0.0.1").empty());
  ::close(fd);
}

TEST_CASE("transcode resamples a pcm source into the aac stream") {
  // pcm_s16le at 4000 Hz mono: not copyable, needs decode + rate clamp +
  // channel layout conversion + encode — and the fifo reframing of a
  // decoder that delivers 4000-sample frames into 1024-sample ones.
  cast::TranscodeServer ts;
  std::string err;
  REQUIRE(ts.start(castFixture("cast_pcm_aud.mov"), &err));
  REQUIRE_MESSAGE(ts.waitReady(20000), ts.lastError());

  const StreamFetch got = fetchStream("127.0.0.1", ts.port());
  CHECK(got.status == 200);
  CHECK(got.body.size() > 2000);
  CHECK(tsSync(got.body));
  const std::string codecs = probeCodecs(got.body);
  if (!codecs.empty()) {
    CHECK(codecs.find("aac") != std::string::npos);
    CHECK(codecs.find("h264") == std::string::npos);  // audio-only source
  }
  ts.stop();
}

TEST_CASE("transcode reports a source with no playable streams") {
  // A timecode-only track: containers parse, nothing transcodes.
  cast::TranscodeServer ts;
  std::string err;
  REQUIRE(ts.start(castFixture("cast_data_only.mov"), &err));
  REQUIRE_MESSAGE(ts.waitReady(20000) == false, ts.lastError());
  // A failed text match is undiagnosable without the actual verdict.
  CHECK_MESSAGE(ts.lastError().find("no video or audio stream") != std::string::npos,
                ts.lastError());
  ts.stop();
}

TEST_CASE("a transcode session on an undecodable source fails before Play") {
  // The pump fails on the garbage bytes before the renderer ever sees a
  // URI, and start() reports that instead of a half-cast state.
  const SoloStore store = makeSolo("tsjunk");
  p2p::TorrentStream bridge;
  p2p::TorrentStream::Params bp;
  bp.torrent_path = store.torrent;
  bp.store_dir = store.dir;
  REQUIRE(bridge.start(bp));
  REQUIRE(bridge.phase() == p2p::TorrentPhase::Serving);

  FakeRenderer fr({});
  cast::CastSession session;
  cast::CastSession::Params params;
  params.bridge = &bridge;
  params.renderer_location = fr.location();
  params.transcode = true;
  REQUIRE_FALSE(session.start(params));
  CHECK(session.lastError().find("could not start on the source") !=
        std::string::npos);
  CHECK(session.phase() == cast::CastSession::Phase::Failed);
  // The bridge exposure is undone on the failure path.
  CHECK(bridge.playbackUrl().rfind("http://127.0.0.1:", 0) == 0);
  session.stop();
}

TEST_CASE("a stalled renderer fails a transcode session naming the TS stream") {
  // Real media (the pump readies), a renderer that fetches the URI but
  // never plays: the stall diagnosis must name the transcoded stream, not
  // tell the caller to enable transcoding.
  SoloStore store;
  store.dir = makeTempDir("tsstall");
  store.bytes = readFixture("cast_h264_aac.mp4");
  writeBytes(store.dir + "/media.bin", store.bytes);
  store.torrent = buildTorrent(store.dir, store.dir + "/media.torrent");

  p2p::TorrentStream bridge;
  p2p::TorrentStream::Params bp;
  bp.torrent_path = store.torrent;
  bp.store_dir = store.dir;
  REQUIRE(bridge.start(bp));
  REQUIRE(bridge.phase() == p2p::TorrentPhase::Serving);

  FakeRenderer::Config config;
  config.stalled = true;
  FakeRenderer fr(config);

  cast::CastSession session;
  cast::CastSession::Params params;
  params.bridge = &bridge;
  params.renderer_location = fr.location();
  params.transcode = true;
  params.poll_ms = 100;
  params.start_grace_ms = 800;
  REQUIRE_MESSAGE(session.start(params), "start failed: " << session.lastError());
  CHECK(waitPhase(session, cast::CastSession::Phase::Failed, 15000));
  CHECK(session.lastError().find("stalled even") != std::string::npos);
  CHECK(session.lastError().find("transcoded") != std::string::npos);
  CHECK(bridge.playbackUrl().rfind("http://127.0.0.1:", 0) == 0);
  session.stop();
}

#endif  // SOAR_WITH_FFMPEG

#endif  // _WIN32
