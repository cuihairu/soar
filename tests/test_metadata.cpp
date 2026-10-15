// Unit tests for the network metadata scraper (docs/mvp.md §3, the network
// half of "媒体库与刮削"): the TMDB-shaped search client behind a
// user-configured endpoint and key (src/core/metadata_scraper.*).
//
// Everything here is pure logic plus one temp directory. The wire cases need
// a POSIX fork and python3: they run against a local fixture server
// (test_http_servers.h), never against the real network - which is also
// what makes the "unconfigured means no network" arc assertable.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "soar/core/metadata_scraper.h"

#include "test_http_servers.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef _WIN32
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

using namespace std::chrono_literals;
using soar::HttpMetadataConfig;
using soar::HttpMetadataScraper;
using soar::ScrapedMetadata;

namespace {

// Scratch dir per process; POSIX uses mkdtemp, Windows a fixed path under
// %TEMP% (mirrors tests/test_subtitle.cpp).
struct TempDir {
  std::string path;

  TempDir() {
#ifdef _WIN32
    path = (std::filesystem::temp_directory_path() / "soar_metadata_test").string();
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
#else
    std::string tmpl = "/tmp/soar_metadata_test_XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    const char* dir = ::mkdtemp(buf.data());
    path = dir ? std::string(dir) : std::string(".");
#endif
  }

  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }

  std::string file(const std::string& name) const {
    return (std::filesystem::path(path) / name).string();
  }
};

// The fixture server's requests.log, one tab-separated line per request:
// path, X-API-Key, decoded query. Empty when no request was ever recorded -
// which is the assertion that backs "unconfigured touches no network".
std::vector<std::string> readRequests(const TempDir& root) {
  std::vector<std::string> out;
  std::ifstream in(root.file("requests.log"));
  if (!in) return out;
  std::string line;
  while (std::getline(in, line)) out.push_back(std::move(line));
  return out;
}

// Field `field` (0-based) of line `i` of the log, split on '\t'. The last
// field ends at the end of the line, so the field count is one fewer than
// the tab count - the delimiter before a field, not the one after it.
std::string join(const std::vector<std::string>& lines, size_t i, size_t field) {
  if (i >= lines.size()) return {};
  const std::string& l = lines[i];
  size_t start = 0;
  for (size_t f = 0; f < field; ++f) {
    const size_t tab = l.find('\t', start);
    if (tab == std::string::npos) return {};
    start = tab + 1;
  }
  const size_t end = l.find('\t', start);
  return end == std::string::npos ? l.substr(start)
                                  : l.substr(start, end - start);
}

// Unescapes the percent-encoding a query string travels in, so a test can
// compare against the title it sent.
std::string unescape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  char buf[3] = {0, 0, 0};
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      const auto hi = [&](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      const int a = hi(s[i + 1]);
      const int b = hi(s[i + 2]);
      if (a >= 0 && b >= 0) {
        out.push_back(static_cast<char>((a << 4) | b));
        i += 2;
        continue;
      }
    }
    out.push_back(s[i]);
  }
  return out;
}

}  // namespace

// =========================================================================
// Configuration and the offline contract
// =========================================================================

TEST_CASE("metadata_scraper_config_drives_the_offline_contract") {
  SUBCASE("an unconfigured scraper is offline") {
    HttpMetadataScraper s;
    CHECK_FALSE(s.online());
  }

  SUBCASE("a configured http endpoint is online") {
    HttpMetadataScraper s;
    HttpMetadataConfig cfg;
    cfg.endpoint = "http://127.0.0.1:9";
    s.configure(cfg);
    CHECK(s.online());
  }

  SUBCASE("https is not honored - the client speaks no TLS") {
    HttpMetadataScraper s;
    HttpMetadataConfig cfg;
    cfg.endpoint = "https://api.example.org";
    s.configure(cfg);
    CHECK_FALSE(s.online());
  }

  SUBCASE("scraping offline fails without touching a network") {
    HttpMetadataScraper s;
    ScrapedMetadata out{"untouched", "untouched"};
    std::string err;
    CHECK_FALSE(s.scrape("The Test", &out, &err));
    CHECK(!err.empty());
    // Failure leaves the output untouched, like SubtitleTranslator.
    CHECK(out.title == "untouched");
    CHECK(out.plot == "untouched");
  }

  SUBCASE("the default timeout is 5 s") {
    HttpMetadataConfig cfg;
    CHECK(cfg.timeout == 5000ms);
    CHECK(cfg.api_key.empty());
  }

  SUBCASE("null pointers are tolerated") {
    HttpMetadataScraper s;
    CHECK_FALSE(s.scrape("The Test", nullptr, nullptr));
  }

  SUBCASE("configure replaces the whole setting") {
    HttpMetadataScraper s;
    HttpMetadataConfig cfg;
    cfg.endpoint = "http://127.0.0.1:1";
    cfg.api_key = "key-1";
    s.configure(cfg);
    CHECK(s.online());
    HttpMetadataConfig empty;
    s.configure(empty);
    CHECK_FALSE(s.online());
  }

  // The url parser is the only thing between a user-typed endpoint and the
  // socket call. Every shape it refuses must fail before a connection is
  // even attempted - no network in these cases, no fixture server.
  SUBCASE("urls the parser cannot use fail without a connection") {
    const char* urls[] = {
        "http://",                  // no host at all
        "http://[::1:1/",           // bracket never closed
        "http://[::1]x80/",         // junk after the bracket
        "http://127.0.0.1:12ab/"};  // a non-numeric port
    for (const char* url : urls) {
      HttpMetadataScraper s;
      HttpMetadataConfig cfg;
      cfg.endpoint = url;
      cfg.timeout = 2000ms;
      s.configure(cfg);
      REQUIRE(s.online());
      std::string err;
      CHECK_FALSE(s.scrape("Anything", nullptr, &err));
      CHECK(err.find("not a usable http:// url") != std::string::npos);
    }
  }

  SUBCASE("a bracketed ipv6 literal reaches the socket by address") {
    HttpMetadataScraper s;
    HttpMetadataConfig cfg;
    cfg.endpoint = "http://[::1]:1/";
    cfg.timeout = 2000ms;
    s.configure(cfg);
    std::string err;
    CHECK_FALSE(s.scrape("Anything", nullptr, &err));
    // Loopback refuses quickly, so this stays an instant no-metadata.
    CHECK(err.find("cannot connect") != std::string::npos);
  }

  SUBCASE("a host that cannot resolve fails without hanging") {
    HttpMetadataScraper s;
    HttpMetadataConfig cfg;
    // A single label longer than 63 octets is rejected by the resolver
    // before any query goes out, so this neither needs DNS nor can it hang
    // on a connect retry like a name the sandbox happens to resolve.
    cfg.endpoint = "http://" + std::string(300, 'a') + "/";
    cfg.timeout = 2000ms;
    s.configure(cfg);
    std::string err;
    CHECK_FALSE(s.scrape("Anything", nullptr, &err));
    CHECK(err.find("cannot resolve") != std::string::npos);
  }
}

// =========================================================================
// Wire arcs against the local fixture server (POSIX + python3)
// =========================================================================

TEST_CASE("metadata_scraper_searches_and_reads_the_first_hit") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping metadata tests");
    return;
  }
  const TempDir root;
  const test_servers::RangeServer srv =
      test_servers::startMetadataServer(root.path, "ok", "sekret-key");
  REQUIRE(srv.pid >= 0);

  HttpMetadataScraper s;
  HttpMetadataConfig cfg;
  cfg.endpoint = srv.base_url;
  cfg.api_key = "sekret-key";
  cfg.timeout = 5000ms;
  s.configure(cfg);
  REQUIRE(s.online());

  ScrapedMetadata out;
  std::string err;
  REQUIRE(s.scrape("The Movie", &out, &err));

  // The first result's title and plot; the second result and every field
  // the scraper does not carry (dates, posters, ratings) are ignored.
  CHECK(out.title == "Café & 😀");
  CHECK(out.plot == "A ‘plot’ of sorts");

  // The request the client produced: the documented path, the key in a
  // header (never in the url), the title percent-encoded in the query.
  const std::vector<std::string> reqs = readRequests(root);
  REQUIRE(reqs.size() == 1);
  CHECK(join(reqs, 0, 0) == "/search/movie?query=The%20Movie");
  CHECK(join(reqs, 0, 1) == "sekret-key");
  CHECK(unescape(join(reqs, 0, 2)) == "The Movie");

  SUBCASE("a space and a non-ASCII title ride as utf-8 bytes") {
    const TempDir root2;
    const test_servers::RangeServer srv2 =
        test_servers::startMetadataServer(root2.path, "utf8", "");
    REQUIRE(srv2.pid >= 0);
    HttpMetadataConfig cfg2;
    cfg2.endpoint = srv2.base_url;
    cfg2.timeout = 5000ms;
    HttpMetadataScraper s2(cfg2);
    ScrapedMetadata out2;
    REQUIRE(s2.scrape("Café 你好", &out2, nullptr));
    // É = C3 A9, 你 = E4 BD A0, 好 = E5 A5 BD.
    CHECK(join(readRequests(root2), 0, 0) ==
          "/search/movie?query=Caf%C3%A9%20%E4%BD%A0%E5%A5%BD");
    CHECK(out2.title == "Café 你好");
  }

  SUBCASE("a lone surrogate becomes U+FFFD, never invalid utf-8") {
    const TempDir root3;
    const test_servers::RangeServer srv3 =
        test_servers::startMetadataServer(root3.path, "lonesur", "");
    REQUIRE(srv3.pid >= 0);
    HttpMetadataConfig cfg3;
    cfg3.endpoint = srv3.base_url;
    cfg3.timeout = 5000ms;
    HttpMetadataScraper s3(cfg3);
    ScrapedMetadata out3;
    REQUIRE(s3.scrape("Lone", &out3, nullptr));
    CHECK(out3.title == "Lone \xEF\xBF\xBD surrogate");
  }

  SUBCASE("a lone low surrogate is U+FFFD too, and an unpaired \\u is not a pair") {
    const TempDir root4;
    const test_servers::RangeServer srv4 =
        test_servers::startMetadataServer(root4.path, "lowsur", "");
    REQUIRE(srv4.pid >= 0);
    HttpMetadataConfig cfg4;
    cfg4.endpoint = srv4.base_url;
    cfg4.timeout = 5000ms;
    HttpMetadataScraper s4(cfg4);
    ScrapedMetadata out4;
    REQUIRE(s4.scrape("Lone", &out4, nullptr));
    CHECK(out4.title == "Lone \xEF\xBF\xBD surrogate");
  }
#endif
}

TEST_CASE("metadata_scraper_rejects_answers_it_cannot_read") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping metadata tests");
    return;
  }
  const TempDir root;
  const test_servers::RangeServer srv =
      test_servers::startMetadataServer(root.path, "notitle", "");
  REQUIRE(srv.pid >= 0);

  HttpMetadataScraper s;
  HttpMetadataConfig cfg;
  cfg.endpoint = srv.base_url;
  cfg.timeout = 5000ms;
  s.configure(cfg);

  struct Shape {
    const char* mode;
    bool has_title;   // a "title" the client can read as a string
    bool non_empty;   // and non-empty
  };
  const Shape shapes[] = {
      {"emptyresults", false, false},
      {"notitle", false, false},
      {"emptytitle", true, false},
      {"nonstring", false, false},
      {"badjson", false, false},
      {"badescape", false, false},
      {"status500", false, false},
      {"status404", false, false},
  };
  for (const Shape& sh : shapes) {
    const TempDir r;
    const test_servers::RangeServer s2 =
        test_servers::startMetadataServer(r.path, sh.mode, "");
    REQUIRE(s2.pid >= 0);
    HttpMetadataConfig c2;
    c2.endpoint = s2.base_url;
    c2.timeout = 5000ms;
    HttpMetadataScraper sc(c2);
    ScrapedMetadata out{"keep", "keep"};
    std::string err;
    CHECK_FALSE(sc.scrape("Anything", &out, &err));
    CHECK(!err.empty());
    CHECK(out.title == "keep");  // untouched on failure
    CHECK(out.plot == "keep");
  }
#endif
}

TEST_CASE("metadata_scraper_honors_the_body_caps") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping metadata tests");
    return;
  }
  const TempDir root;
  const test_servers::RangeServer srv =
      test_servers::startMetadataServer(root.path, "biglen", "");
  REQUIRE(srv.pid >= 0);
  HttpMetadataScraper s;
  HttpMetadataConfig cfg;
  cfg.endpoint = srv.base_url;
  cfg.timeout = 5000ms;
  s.configure(cfg);
  std::string err;
  CHECK_FALSE(s.scrape("Big", nullptr, &err));
  CHECK(err.find("exceeds") != std::string::npos);

  SUBCASE("a body that promises more than it delivers fails") {
    const TempDir r;
    const test_servers::RangeServer s2 =
        test_servers::startMetadataServer(r.path, "shortbody", "");
    REQUIRE(s2.pid >= 0);
    HttpMetadataConfig c2;
    c2.endpoint = s2.base_url;
    c2.timeout = 5000ms;
    HttpMetadataScraper sc(c2);
    std::string e2;
    CHECK_FALSE(sc.scrape("Short", nullptr, &e2));
    CHECK(!e2.empty());
  }

  SUBCASE("a 200 without Content-Length reads to EOF") {
    const TempDir r;
    const test_servers::RangeServer s2 =
        test_servers::startMetadataServer(r.path, "nolength", "");
    REQUIRE(s2.pid >= 0);
    HttpMetadataConfig c2;
    c2.endpoint = s2.base_url;
    c2.timeout = 5000ms;
    HttpMetadataScraper sc(c2);
    ScrapedMetadata out;
    CHECK(sc.scrape("NoLength", &out, nullptr));
    CHECK(out.title == "Café & 😀");
  }
#endif
}

TEST_CASE("metadata_scraper_rejects_a_bad_key_and_survives_a_dead_endpoint") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping metadata tests");
    return;
  }
  SUBCASE("a wrong key is a 401, reported as no metadata") {
    const TempDir root;
    const test_servers::RangeServer srv =
        test_servers::startMetadataServer(root.path, "ok", "sekret-key");
    REQUIRE(srv.pid >= 0);
    HttpMetadataScraper s;
    HttpMetadataConfig cfg;
    cfg.endpoint = srv.base_url;
    cfg.api_key = "wrong";
    cfg.timeout = 5000ms;
    s.configure(cfg);
    std::string err;
    CHECK_FALSE(s.scrape("The Movie", nullptr, &err));
    CHECK(err.find("401") != std::string::npos);
    // The attempt is recorded - the key the client sent is on the wire.
    CHECK(join(readRequests(root), 0, 1) == "wrong");
  }

  SUBCASE("an endpoint that answers nothing gives up on the timeout") {
    const TempDir root;
    const test_servers::RangeServer srv =
        test_servers::startMetadataServer(root.path, "stall", "");
    REQUIRE(srv.pid >= 0);
    HttpMetadataScraper s;
    HttpMetadataConfig cfg;
    cfg.endpoint = srv.base_url;
    cfg.timeout = 500ms;  // far below the fixture's 40 s sleep
    s.configure(cfg);
    std::string err;
    CHECK_FALSE(s.scrape("The Movie", nullptr, &err));
    CHECK(err.find("timeout") != std::string::npos);
  }

  SUBCASE("a host that never answers fails fast") {
    HttpMetadataScraper s;
    HttpMetadataConfig cfg;
    cfg.endpoint = "http://127.0.0.1:1";  // port 1: nothing listens there
    cfg.timeout = 500ms;
    s.configure(cfg);
    std::string err;
    CHECK_FALSE(s.scrape("The Movie", nullptr, &err));
    CHECK(!err.empty());
  }
#endif
}

TEST_CASE("metadata_scraper_configured_without_a_key_sends_no_header") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping metadata tests");
    return;
  }
  const TempDir root;
  const test_servers::RangeServer srv =
      test_servers::startMetadataServer(root.path, "ok", "");
  REQUIRE(srv.pid >= 0);
  HttpMetadataScraper s;
  HttpMetadataConfig cfg;
  cfg.endpoint = srv.base_url;
  cfg.timeout = 5000ms;
  s.configure(cfg);
  ScrapedMetadata out;
  REQUIRE(s.scrape("No Key", &out, nullptr));
  const std::vector<std::string> reqs = readRequests(root);
  REQUIRE(reqs.size() == 1);
  CHECK(join(reqs, 0, 1).empty());  // no X-API-Key header at all
  CHECK(!out.title.empty());
  // A null answer pointer is tolerated on the success path too: the scrape
  // happened, the caller just did not want the copy.
  CHECK(s.scrape("No Key", nullptr, nullptr));
#endif
}

// =========================================================================
// Wire misbehavior: the fixture answers in shapes a real service can
// produce, and the client must come back with a plain "no metadata" - never
// a hang, never a crash, never a half-decoded title.
// =========================================================================

TEST_CASE("metadata_scraper_rejects_wire_shapes_it_cannot_use") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping metadata tests");
    return;
  }

  struct Wire {
    const char* mode;
    const char* err_needle;  // a substring of the expected failure
    std::chrono::milliseconds timeout;
  };
  const Wire wires[] = {
      {"bighead", "headers exceed", 5000ms},
      {"garbage", "malformed status line", 5000ms},
      {"stallbody", "timeout while receiving body", 700ms},
      {"bigbody", "exceeds", 5000ms},
      {"badhex", "no title", 5000ms},
      {"badlowhex", "no title", 5000ms},
  };
  for (const Wire& w : wires) {
    const TempDir root;
    const test_servers::RangeServer srv =
        test_servers::startMetadataServer(root.path, w.mode, "");
    REQUIRE(srv.pid >= 0);
    HttpMetadataScraper s;
    HttpMetadataConfig cfg;
    cfg.endpoint = srv.base_url;
    cfg.timeout = w.timeout;
    s.configure(cfg);
    ScrapedMetadata out{"keep", "keep"};
    std::string err;
    CHECK_FALSE(s.scrape("Anything", &out, &err));
    CHECK(err.find(w.err_needle) != std::string::npos);
    CHECK(out.title == "keep");  // a failed scrape leaves the answer untouched
    CHECK(out.plot == "keep");
  }
#endif
}

TEST_CASE("metadata_scraper_decodes_json_escapes_it_will_read") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping metadata tests");
    return;
  }
  SUBCASE("every escape the JSON vocabulary defines decodes") {
    const TempDir root;
    const test_servers::RangeServer srv =
        test_servers::startMetadataServer(root.path, "escapes", "");
    REQUIRE(srv.pid >= 0);
    HttpMetadataScraper s;
    HttpMetadataConfig cfg;
    cfg.endpoint = srv.base_url;
    cfg.timeout = 5000ms;
    s.configure(cfg);
    ScrapedMetadata out;
    REQUIRE(s.scrape("Escapes", &out, nullptr));
    // \" \\ \/ \b \f \n \r \t, then 你 (a 3-byte code point) and É (the
    // uppercase-hex arm of \uXXXX).
    CHECK(out.title == "a\"b\\c/d\be\ff\ng\rh\ti你jÉ");
  }

  SUBCASE("a high surrogate followed by a non-low escape is U+FFFD") {
    const TempDir root;
    const test_servers::RangeServer srv =
        test_servers::startMetadataServer(root.path, "highnonlow", "");
    REQUIRE(srv.pid >= 0);
    HttpMetadataScraper s;
    HttpMetadataConfig cfg;
    cfg.endpoint = srv.base_url;
    cfg.timeout = 5000ms;
    s.configure(cfg);
    ScrapedMetadata out;
    REQUIRE(s.scrape("High", &out, nullptr));
    // The high half has no code point of its own: U+FFFD, and the A
    // behind it still decodes to 'A'.
    CHECK(out.title == "\xEF\xBF\xBD" "A");
  }
#endif
}
