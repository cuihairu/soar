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
  std::string description;

  std::mutex mu;
  std::string uri;
  std::string title;
  std::string seek_target;
  bool played = false;
  bool stop_requested = false;
  int position_requests = 0;
  int media_fetch_status = 0;
  bool playing = false;
  std::chrono::steady_clock::time_point play_epoch{};
  std::int64_t seek_base_ms = 0;
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
        state->play_epoch = std::chrono::steady_clock::now();
      }
      body = state->fault_play ? fault_body : ok_envelope;
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
          const std::int64_t elapsed = state->playing
                ? std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - state->play_epoch).count() +
                  state->seek_base_ms
                : state->seek_base_ms;
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
    int go_deaf_after = 0;
    bool join_group = false;  // answer loopback M-SEARCH bursts
  };

  explicit FakeRenderer(Config config) {
    state_ = std::make_shared<FakeState>();
    state_->fault_set_uri = config.fault_set_uri;
    state_->fault_play = config.fault_play;
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
    // Wake the blocked syscalls first; close each fd only after its reader
    // thread is joined (close vs a live recvfrom/accept is a data race).
    if (lfd_ >= 0) ::shutdown(lfd_, SHUT_RDWR);
    if (accept_thread_.joinable()) accept_thread_.join();
    if (lfd_ >= 0) ::close(lfd_);
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
  std::string lastSeekTarget() {
    std::lock_guard<std::mutex> lock(state_->mu);
    return state_->seek_target;
  }
  bool sawPlay() {
    std::lock_guard<std::mutex> lock(state_->mu);
    return state_->played;
  }
  bool sawStop() {
    std::lock_guard<std::mutex> lock(state_->mu);
    return state_->stop_requested;
  }
  int mediaFetchStatus() {
    std::lock_guard<std::mutex> lock(state_->mu);
    return state_->media_fetch_status;
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
      if (cfd < 0) break;  // woken by the destructor's shutdown
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
  std::fprintf(stderr, "degenerate: null-out refusal\n");
  CHECK(cast::discoverRenderers(50, nullptr, &err) == -1);
  // A multicast interface that is not an address.
  std::fprintf(stderr, "degenerate: bad-interface refusal\n");
  CHECK(cast::discoverRenderers(50, &found, &err, "999.999.1.1") == -1);
  std::fprintf(stderr, "degenerate: bad-interface returned\n");
  CHECK(err.find("bad multicast interface") != std::string::npos);
  std::fprintf(stderr, "degenerate: full-window discovery\n");

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
  CHECK(bridge.phase() == p2p::TorrentPhase::Serving);  // the bridge lives on
  CHECK(bridge.playbackUrl().rfind("http://127.0.0.1:", 0) == 0);
  CHECK(getStatus(bridge.playbackUrl()) == 206);
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
  CHECK(session.lastError().find("no sender-side transcoding") != std::string::npos);
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

#endif  // _WIN32
