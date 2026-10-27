// Unit tests for the launcher-intake helper (src/app/uri_decode.*): a
// file:// URI becomes its local path with percent-encoding undone, and
// everything else comes back byte-for-byte. Pure std::string — no display,
// no media fixtures, no filesystem access.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "uri_decode.h"

#include <string>

using soar::app::decodeSourceUri;

TEST_CASE("a launcher file URI decodes to its path") {
  CHECK(decodeSourceUri("file:///home/user/a.mkv") == "/home/user/a.mkv");
  // A desktop encodes every space; the '%' is part of the encoding, not
  // of the name.
  CHECK(decodeSourceUri("file:///home/user/My%20Video.mkv") ==
        "/home/user/My Video.mkv");
  // Multiple, adjacent, and UTF-8 escapes in one path.
  CHECK(decodeSourceUri("file:///tmp/a%20b%20c%E4%BD%A0.mkv") ==
        "/tmp/a b c\xE4\xBD\xA0.mkv");
  // A literal '%' in a name is %25; undoing it yields the literal '%'.
  CHECK(decodeSourceUri("file:///tmp/100%25_done.mkv") == "/tmp/100%_done.mkv");
  CHECK(decodeSourceUri("file://localhost/abs/a.mkv") == "/abs/a.mkv");
  // Windows form: the URI has no authority either, so the parse sees the
  // path /C:/...; on Windows the leading slash is dropped so the drive
  // letter roots the path, on POSIX the /C: directory is taken literally.
#ifdef _WIN32
  CHECK(decodeSourceUri("file:///C:/Users/me/a.mkv") == "C:/Users/me/a.mkv");
#else
  CHECK(decodeSourceUri("file:///C:/Users/me/a.mkv") == "/C:/Users/me/a.mkv");
#endif
  // Scheme matching is case-insensitive; a launcher may spell it either way.
  CHECK(decodeSourceUri("FILE:///tmp/a.mkv") == "/tmp/a.mkv");
  // An empty path is the only input that turns into an empty output.
  CHECK(decodeSourceUri("file://").empty());
}

TEST_CASE("invalid escapes stay literal") {
  CHECK(decodeSourceUri("file:///tmp/a%ZZb.mkv") == "/tmp/a%ZZb.mkv");
  CHECK(decodeSourceUri("file:///tmp/a%.mkv") == "/tmp/a%.mkv");
  // A truncated escape at the end of the string keeps the '%'.
  CHECK(decodeSourceUri("file:///tmp/a%2") == "/tmp/a%2");
}

TEST_CASE("plus is not a space") {
  // Only %XX is encoding; '+' must not become whitespace.
  CHECK(decodeSourceUri("file:///tmp/a+b.mkv") == "/tmp/a+b.mkv");
}

TEST_CASE("a remote file URI is untouched") {
  CHECK(decodeSourceUri("file://192.168.1.5/share/a.mkv") ==
        "file://192.168.1.5/share/a.mkv");
  CHECK(decodeSourceUri("file://user@host/share/a.mkv") ==
        "file://user@host/share/a.mkv");
}

TEST_CASE("non-file sources pass through unchanged") {
  CHECK(decodeSourceUri("/home/user/a.mkv") == "/home/user/a.mkv");
  CHECK(decodeSourceUri("./a.mkv") == "./a.mkv");
  CHECK(decodeSourceUri("a.mkv") == "a.mkv");
  CHECK(decodeSourceUri("C:/Users/me/a.mkv") == "C:/Users/me/a.mkv");
  CHECK(decodeSourceUri("http://127.0.0.1:8080/a.mkv") ==
        "http://127.0.0.1:8080/a.mkv");
  CHECK(decodeSourceUri("https://example.com/a.mkv") == "https://example.com/a.mkv");
  CHECK(decodeSourceUri("magnet:?xt=urn:btih:0123456789abcdef") ==
        "magnet:?xt=urn:btih:0123456789abcdef");
  CHECK(decodeSourceUri("asset://sample") == "asset://sample");
  CHECK(decodeSourceUri("-weird") == "-weird");
  CHECK(decodeSourceUri("").empty());
}

TEST_CASE("launcher URIs parse every authority a file manager can send") {
  // The scheme and the host are matched without case: FILE://LOCALHOST
  // is the same local path as file://localhost.
  CHECK(decodeSourceUri("FILE://LOCALHOST/tmp/a.mkv") == "/tmp/a.mkv");
  CHECK(decodeSourceUri("File://LocalHost/a.mkv") == "/a.mkv");
  // Hex digits decode in either case too.
  CHECK(decodeSourceUri("file:///tmp/%e4%bd%a0.mkv") ==
        "/tmp/\xE4\xBD\xA0.mkv");
  CHECK(decodeSourceUri("file:///tmp/%7e/clip.mkv") == "/tmp/~/clip.mkv");
  // One hex digit, or one that is not a hex digit: not an escape, so the
  // three bytes stay put (the path must not shift left by two characters).
  CHECK(decodeSourceUri("file:///tmp/1%2Z.mkv") == "/tmp/1%2Z.mkv");
  CHECK(decodeSourceUri("file:///tmp/a%9") == "/tmp/a%9");
  // A local authority with no path at all yields nothing to open...
  CHECK(decodeSourceUri("file://localhost").empty());
  CHECK(decodeSourceUri("file://").empty());
  // ...while a remote one is handed back byte for byte.
  CHECK(decodeSourceUri("file://192.168.1.5/share/a.mkv") ==
        "file://192.168.1.5/share/a.mkv");
  CHECK(decodeSourceUri("file://192.168.1.5") == "file://192.168.1.5");
  // Spaces and non-ASCII text survive the decode as raw bytes.
  CHECK(decodeSourceUri("file:///tmp/a b/c.mkv") == "/tmp/a b/c.mkv");
  CHECK(decodeSourceUri("file:///tmp/\xE4\xB8\xAD\xE6\x96\x87.mkv") ==
        "/tmp/\xE4\xB8\xAD\xE6\x96\x87.mkv");
  // A short or non-file URI never reaches the path scan.
  CHECK(decodeSourceUri("file:/tmp/a.mkv") == "file:/tmp/a.mkv");
  CHECK(decodeSourceUri("file:") == "file:");
}

TEST_CASE("decoding is idempotent") {
  const char* cases[] = {
      "file:///home/user/My%20Video.mkv",
      "/home/user/My Video.mkv",
      "magnet:?xt=urn:btih:0123456789abcdef",
      "file://192.168.1.5/share/a.mkv",
      "file:///tmp/a%ZZb.mkv",
      ""
  };
  for (const char* c : cases) {
    const std::string once = decodeSourceUri(c);
    CHECK(decodeSourceUri(once) == once);
  }
}
