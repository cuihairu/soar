// Unit tests for the external-subtitle core (docs/mvp.md §6): the SRT /
// WebVTT text parser (src/core/subtitle_text.*) and the pluggable
// SubtitleProvider — its local sidecar implementation plus the HTTP
// download provider (src/core/subtitle_provider.*).
//
// The parser and the sidecar provider are pure logic over strings plus one
// temp directory. The download cases need a POSIX fork and python3: they
// run against a local fixture catalog server (test_http_servers.h), never
// against the real network.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "soar/core/ass_dialogue.h"
#include "soar/core/subtitle_provider.h"
#include "soar/core/subtitle_text.h"

#include "test_http_servers.h"

#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef _WIN32
#  include <csignal>
#  include <fcntl.h>
#  include <sys/resource.h>
#  include <unistd.h>
#endif

using namespace std::chrono_literals;
using soar::assDocumentCues;
using soar::detectSubtitleFormat;
using soar::ExternalSubtitleProvider;
using soar::HttpSubtitleConfig;
using soar::kDefaultCueDuration;
using soar::MediaSource;
using soar::mediaHashHex;
using soar::parseSubtitleText;
using soar::readSubtitleFile;
using soar::SidecarSubtitleProvider;
using soar::storeExternalSubtitle;
using soar::SubtitleCandidate;
using soar::SubtitleCue;
using soar::SubtitleFormat;
using soar::SubtitleTranslateConfig;
using soar::SubtitleTranslator;

#ifndef _WIN32
// POSIX-only fixtures: these symbols live inside the shared header's
// #ifndef _WIN32 block, so the using-declarations must be gated too.
using test_servers::RangeServer;
using test_servers::startChatServer;
using test_servers::startRawServer;
using test_servers::startSubtitleServer;
#endif

namespace {

// Scratch dir per process; POSIX uses mkdtemp, Windows a fixed path under
// %TEMP% (mirrors tests/test_ui_state.cpp).
struct TempDir {
  std::string path;

  TempDir() {
#ifdef _WIN32
    path = (std::filesystem::temp_directory_path() / "soar_subtitle_test").string();
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
#else
    std::string tmpl = "/tmp/soar_subtitle_test_XXXXXX";
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

  void write(const std::string& name, const std::string& content) const {
    std::ofstream out(file(name), std::ios::binary);
    out << content;
  }

  void mkdir(const std::string& name) const {
    std::error_code ec;
    std::filesystem::create_directories(file(name), ec);
  }
};

// Run a lambda with the process CWD inside `dir`, then put it back even if
// the case fails.
struct ScopedCwd {
  std::filesystem::path saved;

  explicit ScopedCwd(const std::string& dir) : saved(std::filesystem::current_path()) {
    std::error_code ec;
    std::filesystem::current_path(dir, ec);
  }

  ~ScopedCwd() {
    std::error_code ec;
    std::filesystem::current_path(saved, ec);
  }
};

#ifndef _WIN32
// Lower the soft descriptor limit to the lowest descriptor this process is
// not holding, so that any further open() is refused with EMFILE. Same
// shape as the RLIMIT_FSIZE injection in tests/test_http_servers.h: a real
// refusal from the OS, nothing mocked in the code under test. `probePath`
// has to be an existing readable file — it is what the probe opens, and
// only EMFILE counts as the refusal we came for, so a bogus probe (a file
// that vanished, say) leaves `armed` false and the case fails loudly rather
// than passing for the wrong reason. The limit is put back on every exit
// path, so the rest of the suite runs unconstrained.
struct ScopedFdExhaustion {
  struct rlimit saved {};
  bool lowered = false;
  bool armed = false;

  explicit ScopedFdExhaustion(const std::string& probePath) {
    if (::getrlimit(RLIMIT_NOFILE, &saved) != 0) {
      return;
    }
    // fcntl() is the portable way to ask "is this descriptor in use?".
    rlim_t first_free = 0;
    while (first_free < saved.rlim_cur &&
           ::fcntl(static_cast<int>(first_free), F_GETFD) != -1) {
      ++first_free;
    }
    if (first_free == 0) {
      return;  // not even stdin is held; nothing sensible to exhaust
    }
    struct rlimit next = saved;
    next.rlim_cur = first_free;
    if (::setrlimit(RLIMIT_NOFILE, &next) != 0) {
      return;
    }
    lowered = true;

    const int probe = ::open(probePath.c_str(), O_RDONLY);
    if (probe >= 0) {
      ::close(probe);
      return;  // could not exhaust the descriptors; leave `armed` false
    }
    armed = errno == EMFILE;
  }

  ~ScopedFdExhaustion() {
    if (lowered) {
      ::setrlimit(RLIMIT_NOFILE, &saved);
    }
  }

  ScopedFdExhaustion(const ScopedFdExhaustion&) = delete;
  ScopedFdExhaustion& operator=(const ScopedFdExhaustion&) = delete;
};

// RAII guard around RLIMIT_FSIZE (same shape as the http-cache quota
// guard): a real refusal from the OS, nothing mocked in the code under
// test. The default SIGXFSZ disposition would kill the binary mid-case,
// so the case arms `SIG_IGN` before lowering the limit.
struct ScopedFsizeLimit {
  struct rlimit saved {};
  bool lowered = false;

  explicit ScopedFsizeLimit(rlim_t soft) {
    if (::getrlimit(RLIMIT_FSIZE, &saved) != 0) {
      return;
    }
    struct rlimit next = saved;
    next.rlim_cur = soft;
    lowered = ::setrlimit(RLIMIT_FSIZE, &next) == 0;
  }

  ~ScopedFsizeLimit() {
    if (lowered) {
      ::setrlimit(RLIMIT_FSIZE, &saved);
    }
  }

  ScopedFsizeLimit(const ScopedFsizeLimit&) = delete;
  ScopedFsizeLimit& operator=(const ScopedFsizeLimit&) = delete;
};
#endif

// A UTF-8 BOM. Written as its own literal because "\xEF\xBB\xBF" glued to
// the next line's leading digit would be read as one greedy hex escape
// ("\xBF1"), which is out of range.
const std::string kBom = "\xEF\xBB\xBF";

std::vector<std::string> titlesOf(const std::vector<SubtitleCandidate>& c) {
  std::vector<std::string> out;
  out.reserve(c.size());
  for (const auto& x : c) {
    out.push_back(x.title);
  }
  return out;
}

#ifndef _WIN32
// The catalog the fixture server returns verbatim from /search, plus the
// files its /dl/ serves. The first lines exercise the parser's skip rules
// (comments, malformed lines, an https:// candidate, a format the core
// cannot read); the rest are candidates in catalog order, covering the
// field corners: empty title (falls back to the url), uppercase and alias
// extensions, a five-field line (extras ignored).
void writeCatalog(const TempDir& root, int port) {
  const std::string base = "http://127.0.0.1:" + std::to_string(port);
  std::filesystem::create_directories(root.file("dl"));
  root.write("dl/movie.en.srt",
             "1\n00:00:01,000 --> 00:00:02,500\nhello from the network\n");
  root.write("dl/movie.zh.vtt",
             "WEBVTT\n\n00:00:01.000 --> 00:00:02.500\n\xE5\xAD\x97\xE5\xB9\x95 from the network\n");
  root.write("dl/movie.ass",
             "[Script Info]\n"
             "ScriptType: v4.00+\n"
             "\n"
             "[Events]\n"
             "Format: Layer, Start, End, Style, Name, MarginL, MarginR, "
             "MarginV, Effect, Text\n"
             "Dialogue: 0,0:00:01.00,0:00:02.50,Default,,0,0,0,,"
             "styled from the network\n");
  root.write("dl/movie.ssa",
             "[Script Info]\n"
             "ScriptType: v4.00\n"
             "\n"
             "[Events]\n"
             "Format: Layer, Start, End, Style, Name, MarginL, MarginR, "
             "MarginV, Effect, Text\n"
             "Dialogue: 0,0:00:01.00,0:00:02.50,Default,,0,0,0,,"
             "ssa alias from the network\n");
  root.write("catalog.tsv",
             "# url<TAB>lang<TAB>title<TAB>ext, one candidate per line\n"
             "\n"
             "not a candidate line\n"
             "https://mirror.example.invalid/movie.es.srt\tes\tHTTPS is skipped\tsrt\n"
             "http://127.0.0.1:1/movie.sup\tja\tBitmap subs are skipped\tsup\n"
             + base + "/dl/movie.en.srt\ten\tMovie EN (downloaded)\tsrt\n"
             + base + "/dl/movie.zh.vtt\tzh-Hans\tMovie ZH VTT\tvtt\n"
             + base + "/dl/movie.en.srt\tund\t\tvtt\n"
             + base + "/dl/movie.en.srt\ten-GB\tUK SRT\tSRT\n"
             + base + "/dl/movie.en.srt\tfr\tSubRip alias\tsubrip\n"
             + base + "/dl/movie.en.srt\tde\tFive fields\tvtt\tignored-extra\n"
             + base + "/dl/movie.en.srt\tpt\tBr VTT\tWEBVTT\n"
             + base + "/dl/movie.ass\tja\tStyled ASS\tass\n"
             + base + "/dl/movie.ssa\tko\tStyled SSA\tssa\n");
}
#endif

// =========================================================================
// subtitleFormatFromPath / detectSubtitleFormat
// =========================================================================

TEST_CASE("subtitle_format_from_path") {
  SUBCASE("subrip") {
    CHECK(soar::subtitleFormatFromPath("/v/movie.srt") == SubtitleFormat::SubRip);
    CHECK(soar::subtitleFormatFromPath("MOVIE.SRT") == SubtitleFormat::SubRip);
    CHECK(soar::subtitleFormatFromPath("a.b.en.srt") == SubtitleFormat::SubRip);
  }
  SUBCASE("webvtt") {
    CHECK(soar::subtitleFormatFromPath("/v/movie.vtt") == SubtitleFormat::WebVtt);
    CHECK(soar::subtitleFormatFromPath("movie.webvtt") == SubtitleFormat::WebVtt);
    CHECK(soar::subtitleFormatFromPath("Movie.VTT") == SubtitleFormat::WebVtt);
  }
  SUBCASE("ass/ssa") {
    CHECK(soar::subtitleFormatFromPath("/v/movie.ass") == SubtitleFormat::Ass);
    CHECK(soar::subtitleFormatFromPath("movie.ssa") == SubtitleFormat::Ass);
    CHECK(soar::subtitleFormatFromPath("MOVIE.ASS") == SubtitleFormat::Ass);
  }
  SUBCASE("unsupported") {
    // Bitmap formats need a decoder, not this parser.
    CHECK(soar::subtitleFormatFromPath("movie.sup") == SubtitleFormat::Unknown);
    CHECK(soar::subtitleFormatFromPath("movie") == SubtitleFormat::Unknown);
    CHECK(soar::subtitleFormatFromPath("") == SubtitleFormat::Unknown);
  }
}

TEST_CASE("detect_subtitle_format") {
  SUBCASE("empty") {
    CHECK(detectSubtitleFormat("") == SubtitleFormat::Unknown);
  }
  SUBCASE("webvtt header wins") {
    // The header is the first non-blank line; a BOM in front of it is not
    // part of it, and a description may follow it.
    CHECK(detectSubtitleFormat("\xEF\xBB\xBFWEBVTT\n\n00:00.000 --> 00:01.000\nhi\n") ==
          SubtitleFormat::WebVtt);
    CHECK(detectSubtitleFormat("\nWEBVTT - transcript\nKind: captions\n\n") ==
          SubtitleFormat::WebVtt);
  }
  SUBCASE("fraction separator decides") {
    CHECK(detectSubtitleFormat("1\n00:00:01,000 --> 00:00:02,000\nhi\n") ==
          SubtitleFormat::SubRip);
    CHECK(detectSubtitleFormat("00:00:01.000 --> 00:00:02.000\nhi\n") ==
          SubtitleFormat::WebVtt);
    // A headerless WebVTT is still WebVTT (dot fractions).
    CHECK(detectSubtitleFormat("WEBVTT-ish\n\n00:01.000 --> 00:02.000\n") ==
          SubtitleFormat::WebVtt);
  }
  SUBCASE("a broken arrow line does not decide") {
    // The first "-->" line is not a timestamp; the second one is.
    CHECK(detectSubtitleFormat("x --> y\n00:00:03,000 --> 00:00:04,000\n") ==
          SubtitleFormat::SubRip);
  }
  SUBCASE("ass markers") {
    // Either the script header section or an event line announces ASS.
    CHECK(detectSubtitleFormat("[Script Info]\nScriptType: v4.00+\n") ==
          SubtitleFormat::Ass);
    CHECK(detectSubtitleFormat(
              "Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,hi\n") ==
          SubtitleFormat::Ass);
    // A quoted marker inside cue *text* does not win: the timed arrow
    // line comes first and its separator decides.
    CHECK(detectSubtitleFormat(
              "1\n00:00:01,000 --> 00:00:02,000\nDialogue: not a script\n") ==
          SubtitleFormat::SubRip);
    // A marker line without either ASS marker form still falls through
    // to the arrow scan.
    CHECK(detectSubtitleFormat("[Events]\n") == SubtitleFormat::Unknown);
  }
  SUBCASE("nothing to go on") {
    CHECK(detectSubtitleFormat("just prose\n\nno timestamps here\n") ==
          SubtitleFormat::Unknown);
    CHECK(detectSubtitleFormat("-->") == SubtitleFormat::Unknown);
  }
}

// =========================================================================
// parseSubtitleText — SubRip
// =========================================================================

TEST_CASE("parse_srt_basic") {
  const std::string srt =
      "1\n"
      "00:00:01,000 --> 00:00:04,000\n"
      "Hello world\n"
      "second line\n"
      "\n"
      "2\n"
      "00:01:05,250 --> 00:01:07,000\n"
      "Later\n"
      "\n";
  const std::vector<SubtitleCue> cues = parseSubtitleText(srt);

  REQUIRE(cues.size() == 2);
  CHECK(cues[0].begin == 1000ms);
  CHECK(cues[0].end == 4000ms);
  CHECK(cues[0].text == "Hello world\nsecond line");
  CHECK(cues[1].begin == 65250ms);
  CHECK(cues[1].end == 67000ms);
  CHECK(cues[1].text == "Later");
}

TEST_CASE("parse_srt_crlf_and_bom") {
  // CRLF endings, a UTF-8 BOM prologue, a separator line that is only
  // whitespace, and a file that does not end with a newline.
  const std::string srt =
      kBom +
      "1\r\n"
      "00:00:02,000 --> 00:00:03,500\r\n"
      "With BOM\r\n"
      "\r\n"
      "2\r\n"
      "00:00:04,000 --> 00:00:05,000\r\n"
      "   \r\n"
      "3\r\n"
      "00:00:06,000 --> 00:00:07,000\r\n"
      "Last cue\r\n";
  const std::vector<SubtitleCue> cues = parseSubtitleText(srt);

  // The middle block is a timestamp pair with no payload: dropped, and the
  // cue after it still parses.
  REQUIRE(cues.size() == 2);
  CHECK(cues[0].text == "With BOM");
  CHECK(cues[0].end == 3500ms);
  CHECK(cues[1].text == "Last cue");
}

TEST_CASE("parse_bare_cr_line_endings") {
  // Old Mac line endings, with the CR consumed as the terminator — including
  // a final one with no character behind it.
  const std::vector<SubtitleCue> cues = parseSubtitleText(
      "1\r00:00:01,000 --> 00:00:02,000\rcue one\r\r2\r"
      "00:00:03,000 --> 00:00:04,000\rcue two\r");
  REQUIRE(cues.size() == 2);
  CHECK(cues[0].text == "cue one");
  CHECK(cues[1].text == "cue two");
}

TEST_CASE("parse_timestamp_forms") {
  SUBCASE("no hour field, no fraction") {
    const std::vector<SubtitleCue> cues = parseSubtitleText("00:10 --> 01:30\nshort form\n");
    REQUIRE(cues.size() == 1);
    CHECK(cues[0].begin == 10000ms);
    CHECK(cues[0].end == 90000ms);
  }
  SUBCASE("fraction of one to six digits") {
    const std::vector<SubtitleCue> cues =
        parseSubtitleText("00:00:01,5 --> 00:00:02,25\npadded\n\n"
                          "00:00:03,1234 --> 00:00:04,123456\ntruncated\n");
    REQUIRE(cues.size() == 2);
    CHECK(cues[0].begin == 1500ms);
    CHECK(cues[0].end == 2250ms);
    CHECK(cues[1].begin == 3123ms);
    CHECK(cues[1].end == 4123ms);
  }
  SUBCASE("whitespace around the arrow") {
    const std::vector<SubtitleCue> cues =
        parseSubtitleText("\t00:00:01,000\t  -->  \t00:00:02,000 \t\npadded arrow\n");
    REQUIRE(cues.size() == 1);
    CHECK(cues[0].begin == 1000ms);
    CHECK(cues[0].end == 2000ms);
  }
  SUBCASE("non-numeric hour or minute field rejects the line") {
    CHECK(parseSubtitleText("xx:00:01,000 --> 00:00:02,000\nbad hours\n").empty());
    CHECK(parseSubtitleText("00:xx:01,000 --> 00:00:02,000\nbad minutes\n").empty());
  }
  SUBCASE("empty hour or minute field rejects the line") {
    // A doubled or leading colon leaves the hour/minute field blank; the
    // field parser must refuse it rather than silently reading zero.
    CHECK(parseSubtitleText(":00:01,000 --> 00:00:02,000\nno hours\n").empty());
    CHECK(parseSubtitleText("00::01,000 --> 00:00:02,000\nno minutes\n").empty());
  }
  SUBCASE("an oversized minute field rejects the line") {
    // parseUnsigned bails before its accumulator can overflow; the guard
    // lives on the minute field of a full timestamp too.
    CHECK(parseSubtitleText("00:99999999:01,000 --> 00:00:02,000\nwide minutes\n").empty());
  }
  SUBCASE("hours accumulate") {
    const std::vector<SubtitleCue> cues =
        parseSubtitleText("01:02:03,004 --> 01:02:04,005\ndeep\n");
    REQUIRE(cues.size() == 1);
    CHECK(cues[0].begin == 3723004ms);
    CHECK(cues[0].end == 3724005ms);
  }
  SUBCASE("rejects what is not a timestamp") {
    // No colon at all, a non-numeric hour/minute/second, an empty field, an
    // empty fraction and a field so wide it is a mis-parsed line rather than
    // a timestamp.
    CHECK(parseSubtitleText("00:00:01,000 --> abc\nx\n").empty());
    CHECK(parseSubtitleText("0X:00:01,000 --> 00:00:02,000\nx\n").empty());
    CHECK(parseSubtitleText("00:0X:01,000 --> 00:00:02,000\nx\n").empty());
    CHECK(parseSubtitleText("00:00:0X,000 --> 00:00:02,000\nx\n").empty());
    // A non-digit above '9' takes the other arm of the digit test.
    CHECK(parseSubtitleText("00:00:0a,000 --> 00:00:02,000\nx\n").empty());
    // ... and one below '0' the first arm. Only the edges of a timestamp are
    // trimmed, so a character inside a field survives to the digit test.
    CHECK(parseSubtitleText("00:00:-1,000 --> 00:00:02,000\nx\n").empty());
    CHECK(parseSubtitleText("00::000 --> 00:00:02,000\nx\n").empty());
    CHECK(parseSubtitleText("::000 --> 00:00:02,000\nx\n").empty());
    CHECK(parseSubtitleText("0X:00 --> 00:00:02,000\nx\n").empty());
    CHECK(parseSubtitleText(":00 --> 00:00:02,000\nx\n").empty());
    CHECK(parseSubtitleText("00:00: --> 00:00:02,000\nx\n").empty());
    CHECK(parseSubtitleText("00:00:,500 --> 00:00:02,000\nx\n").empty());
    CHECK(parseSubtitleText("00:00:01, --> 00:00:02,000\nx\n").empty());
    CHECK(parseSubtitleText("00:00:01,0X0 --> 00:00:02,000\nx\n").empty());
    CHECK(parseSubtitleText("00:00:01,0a0 --> 00:00:02,000\nx\n").empty());
    CHECK(parseSubtitleText("00:00:01,0 0 --> 00:00:02,000\nx\n").empty());
    CHECK(parseSubtitleText("99999999:00:01,000 --> 00:00:02,000\nx\n").empty());
  }
}

TEST_CASE("parse_skips_broken_blocks_and_keeps_the_rest") {
  const std::string srt =
      "1\n"
      "not a timestamp line\n"
      "00:00:01,000 --> 00:00:02,000\n"
      "good one\n"
      "\n"
      "2\n"
      "00:0X:01,000 --> 00:00:02,000\n"
      "bad begin\n"
      "\n"
      "3\n"
      "00:00:03,000 --> 00:0X:04,000\n"
      "bad end\n"
      "\n"
      "4\n"
      "00:00:05,000 --> 00:00:06,000\n"
      "\n"
      "5\n"
      "an orphan line\n"
      "also orphan\n"
      "6\n"
      "00:00:09,000 --> 00:00:10,000\n"
      "good two\n";
  const std::vector<SubtitleCue> cues = parseSubtitleText(srt);

  // A stray line, one bad begin, one bad end, a payload-less block and two
  // orphans are all dropped; the surrounding cues survive.
  REQUIRE(cues.size() == 2);
  CHECK(cues[0].text == "good one");
  CHECK(cues[1].text == "good two");
}

TEST_CASE("parse_orphan_line_at_end_of_file") {
  const std::vector<SubtitleCue> cues =
      parseSubtitleText("1\n00:00:01,000 --> 00:00:02,000\nhi\n\norphan");
  REQUIRE(cues.size() == 1);
  CHECK(cues[0].text == "hi");
}

TEST_CASE("parse_skips_junk_before_the_first_cue") {
  const std::vector<SubtitleCue> cues = parseSubtitleText("Downloaded from nowhere\n\n1\n"
                                                          "00:00:01,000 --> 00:00:02,000\n"
                                                          "still parsed\n");
  REQUIRE(cues.size() == 1);
  CHECK(cues[0].text == "still parsed");
}

TEST_CASE("parse_empty_and_hopeless_input") {
  CHECK(parseSubtitleText("").empty());
  CHECK(parseSubtitleText("   \n\n  ").empty());
  CHECK(parseSubtitleText("no timestamps at all").empty());
  // An explicitly requested format still refuses hopeless content.
  CHECK(parseSubtitleText("nothing here", SubtitleFormat::SubRip).empty());
}

TEST_CASE("parse_reversed_and_equal_ranges_get_a_default_duration") {
  const std::vector<SubtitleCue> cues =
      parseSubtitleText("1\n00:00:05,000 --> 00:00:04,000\nbackwards\n\n"
                        "2\n00:00:06,000 --> 00:00:06,000\nzero length\n");
  REQUIRE(cues.size() == 2);
  CHECK(cues[0].end == 5000ms + kDefaultCueDuration);
  CHECK(cues[1].end == 6000ms + kDefaultCueDuration);
  CHECK(cues[0].text == "backwards");
  CHECK(cues[1].text == "zero length");
}

TEST_CASE("parse_a_text_line_containing_an_arrow_ends_the_cue") {
  // Documented limitation: the arrow is the block separator, so a payload
  // line carrying one ends the cue there and is re-read as a block start.
  const std::vector<SubtitleCue> cues =
      parseSubtitleText("1\n00:00:01,000 --> 00:00:02,000\nfirst\n"
                        "second --> still text\n\n"
                        "2\n00:00:03,000 --> 00:00:04,000\nthird\n");
  REQUIRE(cues.size() == 2);
  CHECK(cues[0].text == "first");
  CHECK(cues[1].text == "third");
}

TEST_CASE("parse_cues_without_a_blank_separator") {
  // A missing blank line is not a problem: the arrow line that follows the
  // payload simply opens the next cue.
  const std::vector<SubtitleCue> cues =
      parseSubtitleText("1\n00:00:01,000 --> 00:00:02,000\nfirst\n"
                        "00:00:03,000 --> 00:00:04,000\nsecond");
  REQUIRE(cues.size() == 2);
  CHECK(cues[0].text == "first");
  CHECK(cues[1].text == "second");
}

TEST_CASE("parse_payload_that_looks_like_a_metadata_keyword") {
  // Keyword lines are only metadata at a block start; inside a cue they are
  // payload. (At a block start, parse_webvtt_full drops them.)
  const std::vector<SubtitleCue> cues =
      parseSubtitleText("WEBVTT\n\n00:00:01.000 --> 00:00:02.000\nNOTED: check this\n\n"
                        "00:00:03.000 --> 00:00:04.000\nSTYLE\n");
  REQUIRE(cues.size() == 2);
  CHECK(cues[0].text == "NOTED: check this");
  CHECK(cues[1].text == "STYLE");
}

TEST_CASE("parse_a_cue_runs_until_a_blank_line_or_an_arrow") {
  // Payload is everything up to a blank line or the next arrow line, so an
  // index line that lost its blank separator is read as text.
  const std::vector<SubtitleCue> cues =
      parseSubtitleText("1\n00:00:01,000 --> 00:00:02,000\nNOTED: check this\n2\n"
                        "00:00:03,000 --> 00:00:04,000\nkept\n");
  REQUIRE(cues.size() == 2);
  CHECK(cues[0].text == "NOTED: check this\n2");
  CHECK(cues[1].text == "kept");
}

// =========================================================================
// parseSubtitleText — WebVTT
// =========================================================================

TEST_CASE("parse_webvtt_full") {
  const std::string vtt =
      "WEBVTT\n"
      "Kind: captions\n"
      "Language: en\n"
      "\n"
      "NOTE\n"
      "This file is machine generated.\n"
      "Any arrow -> here stays inside the note.\n"
      "\n"
      "STYLE\n"
      "::cue { color: yellow }\n"
      "\n"
      "REGION\n"
      "id:r1 width:40%\n"
      "\n"
      "intro\n"
      "00:00:01.000 --> 00:00:03.000 align:start position:0%\n"
      "one\n"
      "two\n"
      "\n"
      "00:00:04.000 --> 00:00:05.000\n"
      "no identifier\n";
  const std::vector<SubtitleCue> cues = parseSubtitleText(vtt);

  REQUIRE(cues.size() == 2);
  CHECK(cues[0].begin == 1000ms);
  CHECK(cues[0].end == 3000ms);
  CHECK(cues[0].text == "one\ntwo");  // cue settings ignored
  CHECK(cues[1].text == "no identifier");
}

TEST_CASE("parse_webvtt_metadata_keyword_needs_a_word_boundary") {
  // "NOTES: draft" is a cue identifier, not a NOTE block, and a tab works as
  // well as a space to open one.
  const std::vector<SubtitleCue> cues = parseSubtitleText("WEBVTT\n\nNOTE\ttabbed\n\n"
                                                          "00:00:01.000 --> 00:00:02.000\n"
                                                          "kept\n\n"
                                                          "NOTES: draft\n"
                                                          "00:00:03.000 --> 00:00:04.000\n"
                                                          "also kept\n");
  REQUIRE(cues.size() == 2);
  CHECK(cues[0].text == "kept");
  CHECK(cues[1].text == "also kept");
}

TEST_CASE("parse_webvtt_block_at_end_of_file_without_a_blank_line") {
  // A truncated file: the metadata block runs to EOF and takes nothing else.
  const std::vector<SubtitleCue> cues = parseSubtitleText("WEBVTT\n\nNOTE\nno blank line yet");
  CHECK(cues.empty());
}

TEST_CASE("parse_webvtt_stray_bom_mid_file") {
  const std::vector<SubtitleCue> cues =
      parseSubtitleText("WEBVTT\n\n1\n" + kBom + "00:00:01.000 --> 00:00:02.000\nkept");
  REQUIRE(cues.size() == 1);
  CHECK(cues[0].text == "kept");
}

TEST_CASE("parse_explicit_format_overrides_detection") {
  // Asking for SubRip on WebVTT-looking content: keyword blocks are not
  // skipped (that is a WebVTT rule), so "WEBVTT" is dropped as a stray line
  // and the cues still parse.
  const std::vector<SubtitleCue> cues =
      parseSubtitleText("WEBVTT\n\n00:00:01,000 --> 00:00:02,000\nkept\n",
                        SubtitleFormat::SubRip);
  REQUIRE(cues.size() == 1);
  CHECK(cues[0].text == "kept");
}

TEST_CASE("parse_bom_only_file_is_empty") {
  CHECK(parseSubtitleText("\xEF\xBB\xBF").empty());
  CHECK(parseSubtitleText("\xEF\xBB\xBF\r\n\xEF\xBB\xBF").empty());
}

// =========================================================================
// readSubtitleFile
// =========================================================================

TEST_CASE("read_subtitle_file") {
  const TempDir tmp;
  tmp.write("read.srt", "1\n00:00:01,000 --> 00:00:02,000\nhi\n");
  std::string out;
  REQUIRE(readSubtitleFile(tmp.file("read.srt"), out));
  CHECK(out == "1\n00:00:01,000 --> 00:00:02,000\nhi\n");

  SUBCASE("empty file") {
    tmp.write("empty.vtt", "");
    std::string empty;
    REQUIRE(readSubtitleFile(tmp.file("empty.vtt"), empty));
    CHECK(empty.empty());
  }
  SUBCASE("missing file leaves the output untouched") {
    std::string untouched = "previous";
    CHECK_FALSE(readSubtitleFile(tmp.file("nope.srt"), untouched));
    CHECK(untouched == "previous");
  }
  SUBCASE("a directory is not a subtitle file") {
    tmp.mkdir("adir.srt");
    CHECK_FALSE(readSubtitleFile(tmp.file("adir.srt"), out));
  }
  SUBCASE("a file the OS refuses to open is not a subtitle file") {
    // is_regular_file() stats, so it still says yes while open() fails --
    // exactly the state the defensive arm in readSubtitleFile() exists for.
    // Windows has no RLIMIT_NOFILE; the Linux coverage job supplies it.
#ifndef _WIN32
    tmp.write("refused.srt", "1\n00:00:01,000 --> 00:00:02,000\nhi\n");
    std::string untouched = "previous";
    {
      const ScopedFdExhaustion noDescriptorsLeft(tmp.file("refused.srt"));
      REQUIRE(noDescriptorsLeft.armed);
      CHECK_FALSE(readSubtitleFile(tmp.file("refused.srt"), untouched));
      CHECK(untouched == "previous");
    }
    // The limit really is back, so the injection cannot poison the rest of
    // the suite: the very same file reads normally again.
    CHECK(readSubtitleFile(tmp.file("refused.srt"), out));
    CHECK(out == "1\n00:00:01,000 --> 00:00:02,000\nhi\n");
#endif
  }
}

// =========================================================================
// assDocumentCues — external .ass/.ssa extraction (docs/mvp.md §6, batch 1b)
// =========================================================================

TEST_CASE("ass_document_cues_basic") {
  const std::string doc =
      "[Script Info]\n"
      "ScriptType: v4.00+\n"
      "PlayResX: 640\n"
      "PlayResY: 480\n"
      "\n"
      "[V4+ Styles]\n"
      "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
      "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, "
      "ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
      "Alignment, MarginL, MarginR, MarginV, Encoding\n"
      "Style: Default,Arial,20,&H00FFFFFF,&H000000FF,&H00000000,&H7F000000,"
      "0,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1\n"
      "\n"
      "[Events]\n"
      "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, "
      "Effect, Text\n"
      "Dialogue: 0,0:00:01.00,0:00:03.50,Default,,0,0,0,,Hello\n"
      "Dialogue: 0,0:00:04.00,0:00:06.00,Default,,0,0,0,,second line\n";

  const std::vector<SubtitleCue> cues = assDocumentCues(doc);
  REQUIRE(cues.size() == 2);
  CHECK(cues[0].begin == 1000ms);
  CHECK(cues[0].end == 3500ms);
  CHECK(cues[0].text == "Hello");
  CHECK(cues[1].begin == 4000ms);
  CHECK(cues[1].text == "second line");
}

TEST_CASE("ass_document_cues_text_transforms") {
  // Text is field ten: the commas inside it do not split fields. Override
  // blocks are directives, not content; \N/\n are hard breaks; \h is a
  // hard space; unknown backslash sequences pass through untouched.
  const std::string doc =
      "[Events]\n"
      "Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,a, b, and c\n"
      "Dialogue: 0,0:00:03.00,0:00:04.00,Default,,0,0,0,,{\\pos(320,50)}placed{\\i1}lean{\\i0}\n"
      "Dialogue: 0,0:00:05.00,0:00:06.00,Default,,0,0,0,,top\\Nbottom\\nside\\hjoint\n"
      "Dialogue: 0,0:00:07.00,0:00:08.00,Default,,0,0,0,,kept\\q2 tail\n"
      "Dialogue: 0,0:00:09.00,0:00:10.00,Default,,0,0,0,,dies here {never closed\n";

  const std::vector<SubtitleCue> cues = assDocumentCues(doc);
  REQUIRE(cues.size() == 5);
  CHECK(cues[0].text == "a, b, and c");
  CHECK(cues[1].text == "placedlean");
  CHECK(cues[2].text == "top\nbottom\nside joint");
  CHECK(cues[3].text == "kept\\q2 tail");
  // An unclosed '{' drops the rest of the line, matching how the embedded
  // rect extraction treats a broken directive; what a cue has left over is
  // kept only when it is not blank.
  CHECK(cues[4].text == "dies here");
}

TEST_CASE("ass_document_cues_tolerates_broken_lines") {
  const std::string doc =
      "; a comment\n"
      "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
      "Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,fine\n"
      "Dialogue: 0,0:00:02.00,Default\n"                       // 1 comma: junk
      "Dialogue: 0,0:0:03.00,0:00:04.00,D,,0,0,0,,bad minutes\n"  // bad timestamp
      "Dialogue: 0,0:00:05.00,0:00:04.00,D,,0,0,0,,inverted\n"     // end <= begin
      "Dialogue: 0,0:00:06.00,0:00:07.00,D,,0,0,0,,{\\k30}\n"      // empty text
      "Dialogue: 0,0:00:08.00,0:00:09.00,D,,0,0,0,,  \n";          // blank text

  const std::vector<SubtitleCue> cues = assDocumentCues(doc);
  REQUIRE(cues.size() == 2);
  CHECK(cues[0].text == "fine");
  // An inverted range gets the same default display time the plain-text
  // parser grants.
  CHECK(cues[1].text == "inverted");
  CHECK(cues[1].begin == 5000ms);
  CHECK(cues[1].end == cues[1].begin + kDefaultCueDuration);
}

TEST_CASE("ass_document_cues_crlf_and_hour_widths") {
  const std::string doc =
      "[Script Info]\r\n"
      "Dialogue: 0,1:00:00.50,1:00:02.25,D,,0,0,0,,one hour in\r\n"
      "Dialogue: 0,10:00:00.00,10:00:01.00,D,,0,0,0,,wide hours\r\n";

  const std::vector<SubtitleCue> cues = assDocumentCues(doc);
  REQUIRE(cues.size() == 2);
  CHECK(cues[0].begin == 3600500ms);
  CHECK(cues[0].end == 3602250ms);
  CHECK(cues[0].text == "one hour in");
  CHECK(cues[1].begin == 36000000ms);
  CHECK(cues[1].text == "wide hours");
}

TEST_CASE("ass_document_cues_timestamp_and_field_edges") {
  // Every tolerant-timestamp branch and Dialogue-field edge has a pinned
  // shape: the malformed lines are dropped whole, the merely unusual ones
  // (tabs, a one-digit fraction, a trailing backslash) still cue.
  const std::string doc =
      "Dialogue: 0,\t0:00:01.00,0:00:02.00,D,,0,0,0,,tab start\n"
      "Dialogue: 0,0:00:03.00,0:00:04.00 \t,D,,0,0,0,,tab end\n"
      "Dialogue: 0,0:00:05.00,0:00:06.5,D,,0,0,0,,one digit frac\n"
      "Dialogue:\t0,0:00:07.00,0:00:08.00,D,,0,0,0,,tab after colon\n"
      "Dialogue:\n"
      "Dialogue: 0,0:00:11.00,0:00:12.00,D,,0,0,0,,back\\\n"
      "Dialogue: 0,0:00:13.00,0:00:14.00,D,,0,0,0,,cr\r\r\n"
      "Dialogue: 0,   ,0:00:02.00,D,,0,0,0,,blank start\n"
      "Dialogue: 0,5,0:00:02.00,D,,0,0,0,,no colon\n"
      "Dialogue: 0,1:00,0:00:02.00,D,,0,0,0,,one colon\n"
      "Dialogue: 0,1:00:01,0:00:02.00,D,,0,0,0,,no dot\n"
      "Dialogue: 0,:00:01.00,0:00:02.00,D,,0,0,0,,empty hour\n"
      "Dialogue: 0,0:00:01.00,1:00:1.00,D,,0,0,0,,narrow second\n"
      "Dialogue: 0,0:00:01.00,1:00:01.,D,,0,0,0,,no frac\n"
      "Dialogue: 0,0:00:01.00,1:00:01.0000,D,,0,0,0,,four frac digits\n"
      "Dialogue: 0,x:00:01.00,0:00:02.00,D,,0,0,0,,digit check\n"
      "Dialogue: 0,1:0/:01.00,0:00:02.00,D,,0,0,0,,low digit check\n"
      "Dialogue: 0,1:99:01.00,0:00:02.00,D,,0,0,0,,minute 99\n"
      "Dialogue: 0,1:00:99.00,0:00:02.00,D,,0,0,0,,second 99\n";

  const std::vector<SubtitleCue> cues = assDocumentCues(doc);
  REQUIRE(cues.size() == 6);
  CHECK(cues[0].begin == 1000ms);
  CHECK(cues[0].end == 2000ms);
  CHECK(cues[0].text == "tab start");
  CHECK(cues[1].text == "tab end");
  CHECK(cues[2].end == 6500ms);  // ".5" scales up to 50cs
  CHECK(cues[2].text == "one digit frac");
  CHECK(cues[3].begin == 7000ms);
  CHECK(cues[3].text == "tab after colon");
  CHECK(cues[4].text == "back\\");  // a lone trailing backslash is content
  CHECK(cues[5].text == "cr");      // trailing CR runs inside the text pop
}

TEST_CASE("ass_document_cues_empty_inputs") {
  CHECK(assDocumentCues("").empty());
  CHECK(assDocumentCues("[Script Info]\nonly a header\n").empty());
  CHECK(assDocumentCues("1\n00:00:01,000 --> 00:00:02,000\nsrt\n").empty());
}

// =========================================================================
// SidecarSubtitleProvider
// =========================================================================

TEST_CASE("sidecar_finds_untagged_and_tagged_files") {
  const TempDir tmp;
  tmp.write("movie.mkv", "not really a movie");
  tmp.write("movie.srt", "1\n00:00:01,000 --> 00:00:02,000\nplain\n");
  tmp.write("movie.en.srt", "1\n00:00:01,000 --> 00:00:02,000\nenglish\n");
  tmp.write("movie.zh-Hans.vtt", "WEBVTT\n\n00:00:01.000 --> 00:00:02.000\nchinese\n");
  tmp.write("movie.de.srt", "1\n00:00:01,000 --> 00:00:02,000\ndeutsch\n");
  tmp.write("movie.en.forced.srt", "1\n00:00:01,000 --> 00:00:02,000\nforced\n");

  const SidecarSubtitleProvider provider;
  const std::vector<SubtitleCandidate> c =
      provider.findCandidates(MediaSource{tmp.file("movie.mkv"), {}});

  // Untagged first, then alphabetical among the tagged ones.
  CHECK(titlesOf(c) == (std::vector<std::string>{"movie.srt", "movie.de.srt",
                                                  "movie.en.forced.srt", "movie.en.srt",
                                                  "movie.zh-Hans.vtt"}));

  REQUIRE(c.size() == 5);
  CHECK(c[0].language.empty());
  CHECK(c[0].format == SubtitleFormat::SubRip);
  CHECK(c[0].path == tmp.file("movie.srt"));
  CHECK(c[1].language == "de");
  // "en.forced" contributes only its first tag as a language.
  CHECK(c[2].language == "en");
  CHECK(c[3].path == tmp.file("movie.en.srt"));
  CHECK(c[4].language == "zh-hans");
  CHECK(c[4].format == SubtitleFormat::WebVtt);
}

TEST_CASE("sidecar_matching_is_case_insensitive") {
  const TempDir tmp;
  tmp.write("Movie.MKV", "x");
  tmp.write("MOVIE.EN.SRT", "1\n00:00:01,000 --> 00:00:02,000\nloud\n");
  tmp.write("movie.srt", "1\n00:00:01,000 --> 00:00:02,000\nquiet\n");

  const SidecarSubtitleProvider provider;
  const std::vector<SubtitleCandidate> c =
      provider.findCandidates(MediaSource{tmp.file("Movie.MKV"), {}});
  // Untagged first (the rule is about the tag, not the case), then the
  // tagged one matched through a different letter case.
  CHECK(titlesOf(c) == (std::vector<std::string>{"movie.srt", "MOVIE.EN.SRT"}));
  CHECK(c[0].language.empty());
  CHECK(c[1].language == "en");
}

TEST_CASE("sidecar_skips_what_is_not_a_text_sidecar") {
  const TempDir tmp;
  tmp.write("movie.mkv", "x");
  tmp.write("movie.mp4", "another video");
  tmp.write("movie.sup", "bitmap subs need a decoder, not a parser");
  tmp.write("movie.srt.bak", "not a subtitle extension");
  tmp.write("moviey.srt", "a different movie whose name merely starts the same");
  tmp.write("other.srt", "unrelated");
  tmp.write("srt", "no stem");
  tmp.mkdir("movie.dir.srt");  // a directory that looks like a sidecar

  const SidecarSubtitleProvider provider;
  CHECK(provider.findCandidates(MediaSource{tmp.file("movie.mkv"), {}}).empty());
}

TEST_CASE("sidecar_offers_ass_and_ssa") {
  const TempDir tmp;
  tmp.write("movie.mkv", "x");
  tmp.write("movie.ass", "[Script Info]\nDialogue: 0,0:00:01.00,0:00:02.00,D,,0,0,0,,hi\n");
  tmp.write("movie.ssa", "[Script Info]\nDialogue: 0,0:00:01.00,0:00:02.00,D,,0,0,0,,ho\n");
  tmp.write("movie.ja.ass", "[Script Info]\nDialogue: 0,0:00:01.00,0:00:02.00,D,,0,0,0,,ko\n");

  const SidecarSubtitleProvider provider;
  const std::vector<SubtitleCandidate> c =
      provider.findCandidates(MediaSource{tmp.file("movie.mkv"), {}});
  // Untagged first (movie.ass before movie.ssa by name), then the tagged one.
  CHECK(titlesOf(c) == (std::vector<std::string>{"movie.ass", "movie.ssa",
                                                 "movie.ja.ass"}));
  REQUIRE(c.size() == 3);
  CHECK(c[0].format == SubtitleFormat::Ass);
  CHECK(c[0].language.empty());
  CHECK(c[2].language == "ja");
  CHECK(c[2].format == SubtitleFormat::Ass);
  // The bytes a sidecar hands out detect as the format its extension
  // promised, so loadExternalSubtitle sees a consistent candidate.
  std::string text;
  REQUIRE(provider.fetch(c[0], text));
  CHECK(detectSubtitleFormat(text) == SubtitleFormat::Ass);
}

TEST_CASE("sidecar_media_with_several_dots_in_its_name") {
  const TempDir tmp;
  tmp.write("my.show.s01e02.mkv", "x");
  tmp.write("my.show.s01e02.en.srt", "1\n00:00:01,000 --> 00:00:02,000\nok\n");
  tmp.write("my.show.s01e03.en.srt", "another episode");
  tmp.write("my.show.s01e02.srt", "plain");

  const SidecarSubtitleProvider provider;
  const std::vector<SubtitleCandidate> c =
      provider.findCandidates(MediaSource{tmp.file("my.show.s01e02.mkv"), {}});
  CHECK(titlesOf(c) ==
        (std::vector<std::string>{"my.show.s01e02.srt", "my.show.s01e02.en.srt"}));
}

TEST_CASE("sidecar_accepts_file_urls_and_plain_paths") {
  const TempDir tmp;
  tmp.write("movie.mkv", "x");
  tmp.write("movie.srt", "1\n00:00:01,000 --> 00:00:02,000\nok\n");

  const SidecarSubtitleProvider provider;
  const std::string path = tmp.file("movie.mkv");
  CHECK(provider.findCandidates(MediaSource{"file://" + path, {}}).size() == 1);
  CHECK(provider.findCandidates(MediaSource{path, {}}).size() == 1);
}

TEST_CASE("sidecar_yields_nothing_for_remote_or_missing_media") {
  const TempDir tmp;
  const SidecarSubtitleProvider provider;

  SUBCASE("http and https streams have no sidecar directory") {
    CHECK(provider.findCandidates(MediaSource{"http://example.com/movie.mkv", {}}).empty());
    CHECK(provider.findCandidates(MediaSource{"https://example.com/movie.mkv", {}}).empty());
  }
  SUBCASE("empty uri") {
    CHECK(provider.findCandidates(MediaSource{"", {}}).empty());
  }
  SUBCASE("media that is not there") {
    CHECK(provider.findCandidates(MediaSource{tmp.file("gone.mkv"), {}}).empty());
  }
  SUBCASE("a media path that is a directory") {
    CHECK(provider.findCandidates(MediaSource{tmp.path, {}}).empty());
  }
  SUBCASE("media with no readable parent") {
    CHECK(provider.findCandidates(MediaSource{"/no/such/dir/movie.mkv", {}}).empty());
  }
}

TEST_CASE("sidecar_fetch_reads_the_file") {
  const TempDir tmp;
  tmp.write("movie.mkv", "x");
  tmp.write("movie.srt", "1\n00:00:01,000 --> 00:00:02,000\nfetched\n");

  const SidecarSubtitleProvider provider;
  const std::vector<SubtitleCandidate> c =
      provider.findCandidates(MediaSource{tmp.file("movie.mkv"), {}});
  REQUIRE(c.size() == 1);

  std::string out;
  REQUIRE(provider.fetch(c[0], out));
  // End to end: what the provider hands over is what the parser reads.
  const std::vector<SubtitleCue> cues = parseSubtitleText(out, c[0].format);
  REQUIRE(cues.size() == 1);
  CHECK(cues[0].text == "fetched");

  SUBCASE("a candidate that went away") {
    CHECK_FALSE(provider.fetch(
        SubtitleCandidate{tmp.file("gone.srt"), {}, {}, SubtitleFormat::SubRip}, out));
  }
}

TEST_CASE("sidecar_finds_a_media_opened_by_a_bare_file_name") {
  // A relative uri has an empty parent path, which must still scan the
  // current directory rather than come up empty.
  const TempDir tmp;
  tmp.write("movie.mkv", "x");
  tmp.write("movie.srt", "1\n00:00:01,000 --> 00:00:02,000\nok\n");

  const ScopedCwd cwd(tmp.path);
  const SidecarSubtitleProvider provider;
  CHECK(titlesOf(provider.findCandidates(MediaSource{"movie.mkv", {}})) ==
        std::vector<std::string>{"movie.srt"});
}

// =========================================================================
// ExternalSubtitleProvider — offline and configuration arcs (no server
// needed; these run on every platform)
// =========================================================================

TEST_CASE("unconfigured_provider_stays_offline") {
  // The core never ships an endpoint: an unconfigured provider answers
  // nothing without touching any socket, per the no-forced-networking rule.
  const ExternalSubtitleProvider provider;
  CHECK(provider.findCandidates(MediaSource{"movie.mkv", {}}).empty());

  std::string out;
  CHECK_FALSE(provider.fetch(SubtitleCandidate{
      "http://127.0.0.1:1/x.srt", "en", "x.srt", SubtitleFormat::SubRip}, out));
  CHECK(out.empty());
}

TEST_CASE("configure_swaps_the_config_after_construction") {
  // The default-constructed provider then configured offline still
  // answers nothing; configure() exists so a window can apply environment
  // settings after the member is built.
  ExternalSubtitleProvider provider;
  provider.configure(HttpSubtitleConfig{});  // endpoint stays empty
  CHECK(provider.findCandidates(MediaSource{"movie.mkv", {}}).empty());
}

TEST_CASE("https_endpoint_and_https_candidates_degrade_to_offline") {
  // TLS is deliberately out of scope (see http_cache.h): an https://
  // endpoint is refused locally, before any name lookup or connection,
  // exactly like an unconfigured one.
  const TempDir tmp;
  tmp.write("movie.mkv", std::string(64, 'M'));

  ExternalSubtitleProvider provider;
  HttpSubtitleConfig cfg;
  cfg.endpoint = "https://subtitles.example.invalid/search";
  provider.configure(cfg);
  CHECK(provider.findCandidates(MediaSource{tmp.file("movie.mkv"), {}}).empty());

  std::string out;
  CHECK_FALSE(provider.fetch(SubtitleCandidate{
      "https://mirror.example.invalid/movie.srt", "en", "movie.srt",
      SubtitleFormat::SubRip}, out));
  CHECK(out.empty());
}

TEST_CASE("unreachable_endpoint_degrades_to_no_candidates") {
  // A connection-refused search is just "no external subtitles", not an
  // error — and it must come back promptly, not hang.
  const TempDir tmp;
  tmp.write("movie.mkv", std::string(64, 'M'));

  ExternalSubtitleProvider provider;
  HttpSubtitleConfig cfg;
  cfg.endpoint = "http://127.0.0.1:1/search";  // nothing listens here
  cfg.timeout = 2000ms;
  provider.configure(cfg);
  CHECK(provider.findCandidates(MediaSource{tmp.file("movie.mkv"), {}}).empty());

  std::string out;
  CHECK_FALSE(provider.fetch(SubtitleCandidate{
      "http://127.0.0.1:1/movie.en.srt", "en", "movie.en.srt",
      SubtitleFormat::SubRip}, out));
}

TEST_CASE("media_hash_hex") {
  const TempDir tmp;
  CHECK(mediaHashHex(tmp.file("missing.mkv")).empty());
  tmp.mkdir("adir");
  CHECK(mediaHashHex(tmp.file("adir")).empty());

  // Golden value: 16 bytes 0x00..0x0f are two little-endian words,
  // 0x0706050403020100 + 0x0f0e0d0c0b0a0908 = 0x161412100e0c0a08, plus the
  // size 0x10 — so a service can recompute this digest independently.
  std::string bytes;
  for (int i = 0; i < 16; ++i) {
    bytes.push_back(static_cast<char>(i));
  }
  tmp.write("tiny.bin", bytes);
  CHECK(mediaHashHex(tmp.file("tiny.bin")) == "161412100e0c0a18");

  // An empty file still has a size to hash.
  tmp.write("empty.bin", "");
  CHECK(mediaHashHex(tmp.file("empty.bin")) == "0000000000000000");

  tmp.write("a.bin", std::string(200, 'x'));
  const std::string ha = mediaHashHex(tmp.file("a.bin"));
  CHECK(ha.size() == 16);
  CHECK(ha == mediaHashHex(tmp.file("a.bin")));  // deterministic
  tmp.write("b.bin", std::string(201, 'x'));
  CHECK(ha != mediaHashHex(tmp.file("b.bin")));  // the size counts in

  // Beyond the 64 KiB window the tail chunk joins the sum, so a change in
  // either the head or the tail byte moves the digest.
  std::string big(3 * 64 * 1024, 'q');
  tmp.write("big.bin", big);
  const std::string hb = mediaHashHex(tmp.file("big.bin"));
  CHECK(!hb.empty());
  CHECK(hb == mediaHashHex(tmp.file("big.bin")));
  big[0] = 'r';
  tmp.write("big2.bin", big);
  CHECK(hb != mediaHashHex(tmp.file("big2.bin")));
  big[0] = 'q';
  big[big.size() - 1] = 'z';
  tmp.write("big3.bin", big);
  CHECK(hb != mediaHashHex(tmp.file("big3.bin")));
}

TEST_CASE("store_external_subtitle") {
  const TempDir tmp;
  SubtitleCandidate cand;
  cand.format = SubtitleFormat::SubRip;
  const std::string kText = "1\n00:00:01,000 --> 00:00:02,000\nstored\n";

  SUBCASE("a hostile title is sanitized, the content intact") {
    cand.title = "..\\..\\evil name?.srt";
    const std::string p = storeExternalSubtitle(tmp.path, cand, kText);
    REQUIRE(!p.empty());
    // One component inside <dir>/subtitles/, no separators left to climb
    // out with, and exactly the characters the sanitizer keeps.
    CHECK(std::filesystem::path(p).parent_path() == tmp.file("subtitles"));
    CHECK(std::filesystem::path(p).filename() == ".._.._evil_name_.srt");
    std::string round;
    REQUIRE(readSubtitleFile(p, round));
    CHECK(round == kText);
  }

  SUBCASE("a pure traversal title collapses to a fixed name") {
    cand.title = "..";
    const std::string p = storeExternalSubtitle(tmp.path, cand, kText);
    REQUIRE(!p.empty());
    CHECK(std::filesystem::path(p).filename() == "subtitle.srt");
  }

  SUBCASE("a title without an extension gets one from the format") {
    cand.title = "movie zh";
    const std::string srt = storeExternalSubtitle(tmp.path, cand, kText);
    REQUIRE(!srt.empty());
    CHECK(std::filesystem::path(srt).filename() == "movie_zh.srt");

    cand.format = SubtitleFormat::WebVtt;
    const std::string vtt = storeExternalSubtitle(tmp.path, cand, "WEBVTT\n");
    REQUIRE(!vtt.empty());
    CHECK(std::filesystem::path(vtt).filename() == "movie_zh.vtt");

    cand.format = SubtitleFormat::Ass;
    const std::string ass = storeExternalSubtitle(tmp.path, cand, "[Script Info]\n");
    REQUIRE(!ass.empty());
    CHECK(std::filesystem::path(ass).filename() == "movie_zh.ass");
  }

  SUBCASE("a directory that cannot be created yields empty") {
    tmp.write("plainfile", "not a directory");
    CHECK(storeExternalSubtitle(tmp.file("plainfile"), cand, kText).empty());
  }

  SUBCASE("an empty title survives as subtitle.srt") {
    cand.title = "";
    const std::string p = storeExternalSubtitle(tmp.path, cand, kText);
    REQUIRE(!p.empty());
    CHECK(std::filesystem::path(p).filename() == "subtitle.srt");
  }

  SUBCASE("an empty dir lands in the system temp") {
    cand.title = "temp-store.srt";
    const std::string p = storeExternalSubtitle("", cand, kText);
    REQUIRE(!p.empty());
    CHECK(p.find("subtitles") != std::string::npos);
    std::string round;
    REQUIRE(readSubtitleFile(p, round));
    CHECK(round == kText);
    std::error_code ec;
    std::filesystem::remove(p, ec);  // leave no litter in the shared temp
  }
}

// =========================================================================
// ExternalSubtitleProvider — wire arcs against the local fixture server
// (POSIX + python3, mirroring the test_http_cache.cpp gating)
// =========================================================================

TEST_CASE("search_finds_candidates_and_fetch_downloads_one") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping external subtitle tests");
    return;
  }
  const TempDir root;
  const RangeServer srv = startSubtitleServer(root.path, "catalog", "sekret-key");
  REQUIRE(srv.pid >= 0);
  writeCatalog(root, srv.port);
  root.write("movie.mkv", std::string(4096, 'M'));

  ExternalSubtitleProvider provider;
  HttpSubtitleConfig cfg;
  cfg.endpoint = srv.base_url + "/search";
  cfg.api_key = "sekret-key";
  cfg.timeout = 5000ms;
  provider.configure(cfg);

  // Reaching the catalog at all proves the request carried the right
  // X-API-Key and a query the server validates (size/hash/name present and
  // well formed — anything else is a 400).
  const std::vector<SubtitleCandidate> c =
      provider.findCandidates(MediaSource{root.file("movie.mkv"), {}});
  REQUIRE(c.size() == 9);  // junk, https and bitmap-format lines are skipped
  CHECK(c[0].path == srv.base_url + "/dl/movie.en.srt");
  CHECK(c[0].language == "en");
  CHECK(c[0].title == "Movie EN (downloaded)");
  CHECK(c[0].format == SubtitleFormat::SubRip);
  CHECK(c[1].path == srv.base_url + "/dl/movie.zh.vtt");
  CHECK(c[1].language == "zh-Hans");
  CHECK(c[1].title == "Movie ZH VTT");
  CHECK(c[1].format == SubtitleFormat::WebVtt);
  // Corner rows: an empty title falls back to the url, extensions are
  // matched case-insensitively ("SRT", "WEBVTT") with "subrip" as an
  // alias, and a five-field line keeps its extra columns as noise.
  CHECK(c[2].title == c[2].path);
  CHECK(c[2].format == SubtitleFormat::WebVtt);
  CHECK(c[3].title == "UK SRT");
  CHECK(c[3].format == SubtitleFormat::SubRip);
  CHECK(c[4].title == "SubRip alias");
  CHECK(c[4].format == SubtitleFormat::SubRip);
  CHECK(c[5].title == "Five fields");
  CHECK(c[6].title == "Br VTT");
  CHECK(c[6].format == SubtitleFormat::WebVtt);
  CHECK(c[7].title == "Styled ASS");
  CHECK(c[7].format == SubtitleFormat::Ass);
  CHECK(c[8].title == "Styled SSA");
  CHECK(c[8].format == SubtitleFormat::Ass);  // .ssa shares the ASS mapping

  SUBCASE("fetch returns text the core parser accepts") {
    std::string out;
    REQUIRE(provider.fetch(c[0], out));
    const std::vector<SubtitleCue> cues = parseSubtitleText(out, c[0].format);
    REQUIRE(cues.size() == 1);
    CHECK(cues[0].text == "hello from the network");
  }

  SUBCASE("a downloaded ASS document detects as Ass") {
    std::string out;
    REQUIRE(provider.fetch(c[7], out));
    CHECK(detectSubtitleFormat(out) == SubtitleFormat::Ass);
    const std::vector<SubtitleCue> cues = assDocumentCues(out);
    REQUIRE(cues.size() == 1);
    CHECK(cues[0].text == "styled from the network");
  }

  SUBCASE("a download joins the sidecar pipeline via the store") {
    std::string text;
    REQUIRE(provider.fetch(c[1], text));
    const std::string stored = storeExternalSubtitle(root.path, c[1], text);
    REQUIRE(!stored.empty());
    std::string round;
    REQUIRE(readSubtitleFile(stored, round));
    const std::vector<SubtitleCue> cues =
        parseSubtitleText(round, SubtitleFormat::WebVtt);
    REQUIRE(cues.size() == 1);
    CHECK(cues[0].text == "\xE5\xAD\x97\xE5\xB9\x95 from the network");
  }

  SUBCASE("an endpoint that already carries a query merges with &") {
    ExternalSubtitleProvider p2;
    HttpSubtitleConfig cfg2 = cfg;
    cfg2.endpoint = srv.base_url + "/search?src=unit";
    p2.configure(cfg2);
    CHECK(p2.findCandidates(MediaSource{root.file("movie.mkv"), {}}).size() == 9);
  }
#endif
}

TEST_CASE("wrong_or_missing_api_key_yields_no_candidates") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping external subtitle tests");
    return;
  }
  const TempDir root;
  const RangeServer srv = startSubtitleServer(root.path, "catalog", "sekret-key");
  REQUIRE(srv.pid >= 0);
  writeCatalog(root, srv.port);
  root.write("movie.mkv", std::string(64, 'M'));

  SUBCASE("a wrong key is a 401, reported as no candidates") {
    ExternalSubtitleProvider provider;
    HttpSubtitleConfig cfg;
    cfg.endpoint = srv.base_url + "/search";
    cfg.api_key = "wrong-key";
    provider.configure(cfg);
    CHECK(provider.findCandidates(MediaSource{root.file("movie.mkv"), {}}).empty());
  }

  SUBCASE("no key against a server that wants one degrades the same way") {
    ExternalSubtitleProvider provider;
    HttpSubtitleConfig cfg;
    cfg.endpoint = srv.base_url + "/search";
    provider.configure(cfg);
    CHECK(provider.findCandidates(MediaSource{root.file("movie.mkv"), {}}).empty());
  }
#endif
}

TEST_CASE("server_answer_degrades_to_no_candidates") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping external subtitle tests");
    return;
  }
  const TempDir root;
  root.write("movie.mkv", std::string(64, 'M'));

  std::string mode;
  SUBCASE("an empty catalog is no candidates") {
    mode = "empty";
  }
  SUBCASE("an http 500 is no candidates") {
    mode = "status500";
  }
  SUBCASE("an http 404 is no candidates") {
    mode = "status404";
  }

  const RangeServer srv = startSubtitleServer(root.path, mode, "");
  REQUIRE(srv.pid >= 0);
  ExternalSubtitleProvider provider;
  HttpSubtitleConfig cfg;
  cfg.endpoint = srv.base_url + "/search";
  cfg.timeout = 5000ms;
  provider.configure(cfg);
  CHECK(provider.findCandidates(MediaSource{root.file("movie.mkv"), {}}).empty());
#endif
}

TEST_CASE("a_stalling_server_is_cut_off_by_the_timeout") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping external subtitle tests");
    return;
  }
  const TempDir root;
  // The server accepts, then sleeps 40s before answering. The client's
  // socket timeout — not the test's patience — is what must end the wait.
  const RangeServer srv = startSubtitleServer(root.path, "stall", "");
  REQUIRE(srv.pid >= 0);
  root.write("movie.mkv", std::string(64, 'M'));

  ExternalSubtitleProvider provider;
  HttpSubtitleConfig cfg;
  cfg.endpoint = srv.base_url + "/search";
  cfg.timeout = 300ms;
  provider.configure(cfg);

  const auto t0 = std::chrono::steady_clock::now();
  const std::vector<SubtitleCandidate> c =
      provider.findCandidates(MediaSource{root.file("movie.mkv"), {}});
  const auto elapsed = std::chrono::steady_clock::now() - t0;
  CHECK(c.empty());
  // Generous margin for slow machines, still far below the 40s stall.
  CHECK(elapsed < std::chrono::seconds(10));
#endif
}

TEST_CASE("fetch_rejects_non_subtitle_answers") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping external subtitle tests");
    return;
  }
  const TempDir root;
  const RangeServer srv = startSubtitleServer(root.path, "catalog", "");
  REQUIRE(srv.pid >= 0);
  writeCatalog(root, srv.port);
  root.write("dl/junk.html", "<html><body>not a subtitle</body></html>");

  ExternalSubtitleProvider provider;
  HttpSubtitleConfig cfg;
  cfg.endpoint = srv.base_url + "/search";
  provider.configure(cfg);

  SUBCASE("a missing download is a 404 and a false fetch") {
    std::string out;
    CHECK_FALSE(provider.fetch(SubtitleCandidate{
        srv.base_url + "/dl/gone.srt", "en", "gone.srt",
        SubtitleFormat::SubRip}, out));
    CHECK(out.empty());
  }

  SUBCASE("a 200 body that parses as no subtitle format is a false fetch") {
    // An HTML error page served with 200 must never become a track.
    std::string out;
    CHECK_FALSE(provider.fetch(SubtitleCandidate{
        srv.base_url + "/dl/junk.html", "en", "junk.html",
        SubtitleFormat::SubRip}, out));
    CHECK(out.empty());
  }

  SUBCASE("an unreachable download url is a false fetch") {
    std::string out;
    CHECK_FALSE(provider.fetch(SubtitleCandidate{
        "http://127.0.0.1:1/gone.srt", "en", "gone.srt",
        SubtitleFormat::SubRip}, out));
  }
#endif
}

TEST_CASE("transport_garbage_degrades_to_no_candidates_and_false_fetch") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping external subtitle tests");
    return;
  }
  // A raw socket that answers "NOT-HTTP-AT-ALL" to everything: the client
  // must read it as a failed request, never as content.
  const RangeServer srv = startRawServer("garbage", 25200, 0);
  REQUIRE(srv.pid >= 0);
  const TempDir root;
  root.write("movie.mkv", std::string(64, 'M'));

  ExternalSubtitleProvider provider;
  HttpSubtitleConfig cfg;
  cfg.endpoint = srv.base_url + "/search";
  provider.configure(cfg);
  CHECK(provider.findCandidates(MediaSource{root.file("movie.mkv"), {}}).empty());

  std::string out;
  CHECK_FALSE(provider.fetch(SubtitleCandidate{
      srv.base_url + "/movie.en.srt", "en", "movie.en.srt",
      SubtitleFormat::SubRip}, out));
#endif
}

TEST_CASE("unreadable_media_yields_no_candidates_before_any_dial") {
  // A directory or a missing file has no hash, so the search is refused
  // locally — the endpoint never sees a request for media we cannot
  // identify. The unreachable endpoint makes a stray dial loud (it would
  // still be empty, just via the wrong arm).
  const TempDir tmp;
  tmp.mkdir("adir");

  ExternalSubtitleProvider provider;
  HttpSubtitleConfig cfg;
  cfg.endpoint = "http://127.0.0.1:1/search";
  provider.configure(cfg);
  CHECK(provider.findCandidates(MediaSource{tmp.file("adir"), {}}).empty());
  CHECK(provider.findCandidates(MediaSource{tmp.file("gone.mkv"), {}}).empty());
}

TEST_CASE("provider_constructor_takes_the_config_inline") {
  // The config-taking constructor and the default-ctor-plus-configure
  // pair must land in the same state: an empty endpoint stays offline,
  // an unreachable one still answers with empty candidates.
  const ExternalSubtitleProvider offline(HttpSubtitleConfig{});
  CHECK(offline.findCandidates(MediaSource{"/nonexistent/gone.mkv", {}}).empty());

  HttpSubtitleConfig cfg;
  cfg.endpoint = "http://127.0.0.1:1/search";
  const ExternalSubtitleProvider unreachable(cfg);
  CHECK(unreachable.findCandidates(MediaSource{"/nonexistent/gone.mkv", {}}).empty());
}

TEST_CASE("media_hash_hex_of_an_http_stream_is_empty") {
  // A remote source has no bytes here to hash; the digest is refused
  // locally (with an endpoint configured, no less) before any network
  // question could carry a fake one.
  HttpSubtitleConfig cfg;
  cfg.endpoint = "http://127.0.0.1:1/search";
  ExternalSubtitleProvider provider;
  provider.configure(cfg);
  CHECK(provider.findCandidates(
      MediaSource{"http://example.invalid/movie.mkv", {}}).empty());
}

TEST_CASE("names_with_reserved_characters_are_percent_encoded") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping external subtitle tests");
    return;
  }
  // The search url carries the media's stem; a space or a brace would
  // break the request line raw, so they must come back escaped. The
  // fixture catalog answers whatever the name is, so a full candidate
  // list proves the escaped request was served.
  const TempDir root;
  root.mkdir("dl");
  root.write("dl/movie.en.srt", "1\n00:00:00,000 --> 00:00:01,000\nHi\n");
  const RangeServer srv = startSubtitleServer(root.path, "catalog", "");
  REQUIRE(srv.pid >= 0);
  root.write("catalog.tsv",
             srv.base_url + "/dl/movie.en.srt\ten\tPct srt\tsrt\n");

  ExternalSubtitleProvider provider;
  HttpSubtitleConfig cfg;
  cfg.endpoint = srv.base_url + "/search";
  provider.configure(cfg);

  const std::string media = root.file("weird name {v2}.mkv");
  root.write("weird name {v2}.mkv", "M");
  const std::vector<SubtitleCandidate> candidates =
      provider.findCandidates(MediaSource{media, {}});
  REQUIRE(candidates.size() == 1);
  CHECK(candidates[0].title == "Pct srt");
#endif
}

TEST_CASE("media_hash_hex_returns_empty_when_the_file_cannot_be_opened") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  // A stat succeeds on a file fopen cannot open: descriptor exhaustion
  // sits exactly between the two checks (EMFILE, not ENOENT).
  const TempDir tmp;
  tmp.write("movie.mkv", std::string(64, 'M'));
  ScopedFdExhaustion exhaust(tmp.file("movie.mkv"));
  if (!exhaust.armed) {
    MESSAGE("could not exhaust descriptors; skipping the fopen-failure arm");
    return;
  }
  CHECK(mediaHashHex(tmp.file("movie.mkv")).empty());
#endif
}

TEST_CASE("store_external_subtitle_degrades_when_the_write_quota_is_hit") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  // RLIMIT_FSIZE makes the store's write fail mid-stream — the OS
  // refuses, nothing is mocked. SIGXFSZ rides along with the violation;
  // the default disposition would kill the binary.
  ::signal(SIGXFSZ, SIG_IGN);
  const TempDir tmp;
  const SubtitleCandidate cand{"", "", "Quota srt", SubtitleFormat::SubRip};
  {
    ScopedFsizeLimit limit(32);
    if (!limit.lowered) {
      MESSAGE("could not lower RLIMIT_FSIZE; skipping the quota arm");
      return;
    }
    CHECK(storeExternalSubtitle(tmp.path, cand, std::string(200, 'q')).empty());
  }
  // The quota is restored: the same call now writes through.
  const std::string ok =
      storeExternalSubtitle(tmp.path, cand, std::string(64, 'q'));
  CHECK(!ok.empty());
#endif
}

TEST_CASE("oversized_response_headers_degrade_the_search") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping external subtitle tests");
    return;
  }
  // 70 KB of unterminated header bytes: the 64 KiB header cap fires
  // before the client can mistake the stream for an answer.
  const TempDir root;
  root.write("movie.mkv", std::string(64, 'M'));
  const RangeServer srv = startRawServer("bigheaders", 25200, 0);
  REQUIRE(srv.pid >= 0);

  ExternalSubtitleProvider provider;
  HttpSubtitleConfig cfg;
  cfg.endpoint = srv.base_url + "/search";
  cfg.timeout = std::chrono::milliseconds(3000);
  provider.configure(cfg);
  CHECK(provider.findCandidates(MediaSource{root.file("movie.mkv"), {}}).empty());
#endif
}

TEST_CASE("a_stalled_mid_body_download_times_out") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping external subtitle tests");
    return;
  }
  // A 100-byte body promised, three delivered, the connection held: the
  // receive timeout must end the fetch (a clean close mid-body is the
  // sibling arm, covered by the truncated-download case).
  const RangeServer srv = startRawServer("stallbody", 25200, 0);
  REQUIRE(srv.pid >= 0);

  ExternalSubtitleProvider provider;
  HttpSubtitleConfig cfg;
  cfg.endpoint = srv.base_url + "/search";
  cfg.timeout = std::chrono::milliseconds(300);
  provider.configure(cfg);
  std::string text;
  CHECK_FALSE(provider.fetch(SubtitleCandidate{srv.base_url + "/x.srt", "en",
                                                "x", SubtitleFormat::SubRip},
                              text));
#endif
}

TEST_CASE("malformed_candidate_urls_fail_at_parse_time") {
  // The client speaks http:// only, with a strict authority grammar:
  // bracketed IPv6 (with or without a port), no empty authority, digits
  // only in the port. Each of these is refused before (or while) dialing,
  // never parsed into something else. The default provider is enough —
  // fetch() takes the url from the candidate, not the config.
  const ExternalSubtitleProvider provider;
  std::string out;
  CHECK_FALSE(provider.fetch(SubtitleCandidate{
      "http://[::1]:1/x.srt", "en", "x.srt", SubtitleFormat::SubRip}, out));
  CHECK_FALSE(provider.fetch(SubtitleCandidate{
      "http://[::1]/x.srt", "en", "x.srt", SubtitleFormat::SubRip}, out));
  CHECK_FALSE(provider.fetch(SubtitleCandidate{
      "http://[::1:x]/x.srt", "en", "x.srt", SubtitleFormat::SubRip}, out));
  CHECK_FALSE(provider.fetch(SubtitleCandidate{
      "http:///x.srt", "en", "x.srt", SubtitleFormat::SubRip}, out));
  CHECK_FALSE(provider.fetch(SubtitleCandidate{
      "http://127.0.0.1:notaport/x.srt", "en", "x.srt",
      SubtitleFormat::SubRip}, out));

  // The same grammar guards the endpoint side of the search.
  const TempDir tmp;
  tmp.write("movie.mkv", std::string(64, 'M'));
  ExternalSubtitleProvider p2;
  HttpSubtitleConfig cfg2;
  cfg2.endpoint = "http://[::1:x]/search";
  p2.configure(cfg2);
  CHECK(p2.findCandidates(MediaSource{tmp.file("movie.mkv"), {}}).empty());
  cfg2.endpoint = "http:///search";
  p2.configure(cfg2);
  CHECK(p2.findCandidates(MediaSource{tmp.file("movie.mkv"), {}}).empty());
  cfg2.endpoint = "http://127.0.0.1:notaport/search";
  p2.configure(cfg2);
  CHECK(p2.findCandidates(MediaSource{tmp.file("movie.mkv"), {}}).empty());
}

TEST_CASE("answers_without_content_length_are_read_to_eof") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping external subtitle tests");
    return;
  }
  // HTTP/1.1 with no Content-Length and Connection: close — the body is
  // whatever arrives until EOF. The catalog parses just the same and the
  // download completes.
  const TempDir root;
  const RangeServer srv = startSubtitleServer(root.path, "nolength", "");
  REQUIRE(srv.pid >= 0);
  writeCatalog(root, srv.port);
  root.write("movie.mkv", std::string(64, 'M'));

  ExternalSubtitleProvider provider;
  HttpSubtitleConfig cfg;
  cfg.endpoint = srv.base_url + "/search";
  provider.configure(cfg);
  const std::vector<SubtitleCandidate> c =
      provider.findCandidates(MediaSource{root.file("movie.mkv"), {}});
  REQUIRE(c.size() == 9);

  std::string out;
  REQUIRE(provider.fetch(c[0], out));
  const std::vector<SubtitleCue> cues = parseSubtitleText(out, c[0].format);
  REQUIRE(cues.size() == 1);
  CHECK(cues[0].text == "hello from the network");
#endif
}

TEST_CASE("downloads_past_the_cap_are_refused") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping external subtitle tests");
    return;
  }
  // The server advertises 9 MiB — past the client's 8 MiB download cap —
  // and sends nothing: the refusal must happen before the first body byte,
  // both for a search and for a fetch.
  const TempDir root;
  const RangeServer srv = startSubtitleServer(root.path, "biglen", "");
  REQUIRE(srv.pid >= 0);
  writeCatalog(root, srv.port);
  root.write("movie.mkv", std::string(64, 'M'));

  ExternalSubtitleProvider provider;
  HttpSubtitleConfig cfg;
  cfg.endpoint = srv.base_url + "/search";
  provider.configure(cfg);
  CHECK(provider.findCandidates(MediaSource{root.file("movie.mkv"), {}}).empty());

  std::string out;
  CHECK_FALSE(provider.fetch(SubtitleCandidate{
      srv.base_url + "/dl/big.srt", "en", "big.srt", SubtitleFormat::SubRip},
      out));
  CHECK(out.empty());
#endif
}

TEST_CASE("an_oversized_stream_is_cut_off_midway") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping external subtitle tests");
    return;
  }
  // 9 MiB streamed with no length advertised: the read-to-EOF path must
  // give up at the cap instead of buffering forever.
  const TempDir root;
  const RangeServer srv = startSubtitleServer(root.path, "big", "");
  REQUIRE(srv.pid >= 0);
  writeCatalog(root, srv.port);
  root.write("movie.mkv", std::string(64, 'M'));

  ExternalSubtitleProvider provider;
  HttpSubtitleConfig cfg;
  cfg.endpoint = srv.base_url + "/search";
  provider.configure(cfg);
  CHECK(provider.findCandidates(MediaSource{root.file("movie.mkv"), {}}).empty());

  std::string out;
  CHECK_FALSE(provider.fetch(SubtitleCandidate{
      srv.base_url + "/dl/stream.bin", "en", "stream.bin",
      SubtitleFormat::SubRip}, out));
  CHECK(out.empty());
#endif
}

TEST_CASE("a_truncated_download_is_a_failed_fetch") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping external subtitle tests");
    return;
  }
  // The server promises 100 bytes, sends 3 and hangs up: a clean close
  // mid-body is still a failed fetch, never a partial subtitle.
  const TempDir root;
  const RangeServer srv = startSubtitleServer(root.path, "shortbody", "");
  REQUIRE(srv.pid >= 0);

  ExternalSubtitleProvider provider;
  HttpSubtitleConfig cfg;
  cfg.endpoint = srv.base_url + "/search";
  provider.configure(cfg);
  std::string out;
  CHECK_FALSE(provider.fetch(SubtitleCandidate{
      srv.base_url + "/dl/short.srt", "en", "short.srt",
      SubtitleFormat::SubRip}, out));
  CHECK(out.empty());
#endif
}

TEST_CASE("unwritable_store_targets_fail_cleanly") {
  // Nothing in the store path may throw or create a file outside the
  // subtitles directory, whatever the title or the directory state.
  const TempDir tmp;
  SubtitleCandidate cand;
  cand.format = SubtitleFormat::SubRip;
  const std::string kText = "1\n00:00:01,000 --> 00:00:02,000\nx\n";

  SUBCASE("the subtitles path already exists as a file") {
    tmp.write("subtitles", "not a directory");
    CHECK(storeExternalSubtitle(tmp.path, cand, kText).empty());
  }

  SUBCASE("a title longer than any filesystem allows") {
    cand.title = std::string(300, 'a') + ".srt";
    CHECK(storeExternalSubtitle(tmp.path, cand, kText).empty());
  }
}

// ---------------------------------------------------------------------------
// SubtitleTranslator (docs/mvp.md §6 字幕文本翻译): the OpenAI-compatible
// batch path, driven end-to-end against the local chat fixture server
// (startChatServer, port base 27000). Every failure case asserts a reason
// and an untouched output — the all-or-nothing contract.
// ---------------------------------------------------------------------------

#ifndef _WIN32
// One recorded request from the fixture server's requests.log: path,
// Content-Type, Authorization, body (the client's JSON is a single line,
// which is what makes the log line-oriented).
struct ChatRequest {
  std::string path;
  std::string content_type;
  std::string authorization;
  std::string body;
};

std::vector<ChatRequest> readChatRequests(const TempDir& root) {
  std::vector<ChatRequest> out;
  std::ifstream in(root.file("requests.log"), std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    const size_t p1 = line.find('\t');
    const size_t p2 = line.find('\t', p1 + 1);
    const size_t p3 = line.find('\t', p2 + 1);
    if (p1 == std::string::npos || p2 == std::string::npos ||
        p3 == std::string::npos) {
      continue;
    }
    ChatRequest r;
    r.path = line.substr(0, p1);
    r.content_type = line.substr(p1 + 1, p2 - p1 - 1);
    r.authorization = line.substr(p2 + 1, p3 - p2 - 1);
    r.body = line.substr(p3 + 1);
    out.push_back(std::move(r));
  }
  return out;
}
#endif  // !_WIN32

TEST_CASE("translate_request_shape_and_srt_round_trip") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping translator tests");
    return;
  }
  const TempDir root;
  const RangeServer srv = startChatServer(root.path, "ok", "sekret-key");
  REQUIRE(srv.pid >= 0);

  SubtitleTranslator tr;
  SubtitleTranslateConfig cfg;
  cfg.endpoint = srv.base_url + "/v1";
  cfg.api_key = "sekret-key";
  cfg.model = "test-model";
  cfg.timeout = 5000ms;
  tr.configure(cfg);

  const std::string kIn =
      "1\n00:00:01,000 --> 00:00:02,000\nHello there.\n\n"
      "2\n00:00:03,500 --> 00:00:04,250\nSecond cue line\n\n";
  std::string out, err;
  REQUIRE(tr.translate(kIn, "Japanese", &out, &err));
  CHECK(err.empty());

  // The request: an OpenAI chat-completions POST with the Bearer key, the
  // model, a system message naming the language, and the cues as numbered
  // lines (a literal backslash-n between them — the JSON string escape).
  const std::vector<ChatRequest> reqs = readChatRequests(root);
  REQUIRE(reqs.size() == 1);
  CHECK(reqs[0].path == "/v1/chat/completions");
  CHECK(reqs[0].content_type == "application/json");
  CHECK(reqs[0].authorization == "Bearer sekret-key");
  CHECK(reqs[0].body.find("\"model\":\"test-model\"") != std::string::npos);
  CHECK(reqs[0].body.find("\"temperature\":0") != std::string::npos);
  CHECK(reqs[0].body.find("\"role\":\"system\"") != std::string::npos);
  CHECK(reqs[0].body.find("into Japanese") != std::string::npos);
  CHECK(reqs[0].body.find("1. Hello there.") != std::string::npos);
  CHECK(reqs[0].body.find("\\n2. Second cue line") != std::string::npos);

  // The reply maps back one line per cue: same count, same timings, same
  // SubRip shape (detectable and parseable — i.e. loadable like any
  // sidecar).
  CHECK(detectSubtitleFormat(out) == SubtitleFormat::SubRip);
  const std::vector<SubtitleCue> cues = parseSubtitleText(out, SubtitleFormat::SubRip);
  REQUIRE(cues.size() == 2);
  CHECK(cues[0].text == "[tr] Hello there.");
  CHECK(cues[1].text == "[tr] Second cue line");
  CHECK(cues[0].begin == 1000ms);
  CHECK(cues[0].end == 2000ms);
  CHECK(cues[1].begin == 3500ms);
  CHECK(cues[1].end == 4250ms);
  CHECK(out.find("00:00:01,000 --> 00:00:02,000") != std::string::npos);
  CHECK(out.find("1\n") == 0);  // SubRip numbering follows the file position

  SUBCASE("the translation stores and loads like any sidecar") {
    SubtitleCandidate cand;
    cand.path = "unit.srt";
    cand.title = "movie.zh.srt";
    cand.format = SubtitleFormat::SubRip;
    const std::string stored = storeExternalSubtitle(root.path, cand, out);
    REQUIRE(!stored.empty());
    std::string round;
    REQUIRE(readSubtitleFile(stored, round));
    const std::vector<SubtitleCue> rc = parseSubtitleText(round, SubtitleFormat::SubRip);
    REQUIRE(rc.size() == 2);
    CHECK(rc[1].text == "[tr] Second cue line");
  }
#endif
}

TEST_CASE("translate_vtt_round_trip_keeps_the_format") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping translator tests");
    return;
  }
  const TempDir root;
  const RangeServer srv = startChatServer(root.path, "ok", "");
  REQUIRE(srv.pid >= 0);

  SubtitleTranslator tr;
  SubtitleTranslateConfig cfg;
  cfg.endpoint = srv.base_url + "/v1";
  cfg.model = "test-model";
  tr.configure(cfg);

  const std::string kIn =
      "WEBVTT\n\n00:01.000 --> 00:02.000\nFirst\n\n"
      "00:03.000 --> 00:04.000\nSecond\n\n";
  std::string out, err;
  REQUIRE(tr.translate(kIn, "German", &out, &err));
  // No key configured: no Authorization header must have traveled.
  const std::vector<ChatRequest> reqs = readChatRequests(root);
  REQUIRE(reqs.size() == 1);
  CHECK(reqs[0].authorization.empty());

  CHECK(detectSubtitleFormat(out) == SubtitleFormat::WebVtt);
  CHECK(out.rfind("WEBVTT\n", 0) == 0);
  CHECK(out.find("00:00:01.000 --> 00:00:02.000") != std::string::npos);
  CHECK(out.find(',') == std::string::npos);  // no SubRip commas in a VTT
  const std::vector<SubtitleCue> cues = parseSubtitleText(out, SubtitleFormat::WebVtt);
  REQUIRE(cues.size() == 2);
  CHECK(cues[0].text == "[tr] First");
  CHECK(cues[1].text == "[tr] Second");
#endif
}

TEST_CASE("translate_batches_cues_and_renumbers_each_request") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping translator tests");
    return;
  }
  const TempDir root;
  const RangeServer srv = startChatServer(root.path, "ok", "");
  REQUIRE(srv.pid >= 0);

  SubtitleTranslator tr;
  SubtitleTranslateConfig cfg;
  cfg.endpoint = srv.base_url + "/v1";
  cfg.model = "test-model";
  cfg.batch_cues = 2;  // 3 cues -> two requests, the second renumbered
  tr.configure(cfg);

  const std::string kIn =
      "1\n00:00:01,000 --> 00:00:02,000\nFirst cue\n\n"
      "2\n00:00:02,000 --> 00:00:03,000\nSecond cue\n\n"
      "3\n00:00:03,000 --> 00:00:04,000\nThird cue\n\n";
  std::string out, err;
  REQUIRE(tr.translate(kIn, "French", &out, &err));

  const std::vector<ChatRequest> reqs = readChatRequests(root);
  REQUIRE(reqs.size() == 2);
  CHECK(reqs[0].body.find("1. First cue") != std::string::npos);
  CHECK(reqs[0].body.find("\\n2. Second cue") != std::string::npos);
  CHECK(reqs[1].body.find("\"1. Third cue\"") != std::string::npos);
  CHECK(reqs[1].body.find("\"3.") == std::string::npos);  // numbering restarts

  const std::vector<SubtitleCue> cues = parseSubtitleText(out, SubtitleFormat::SubRip);
  REQUIRE(cues.size() == 3);
  CHECK(cues[0].text == "[tr] First cue");
  CHECK(cues[1].text == "[tr] Second cue");
  CHECK(cues[2].text == "[tr] Third cue");
#endif
}

TEST_CASE("translate_collapses_a_multiline_cue_to_one_wire_line") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping translator tests");
    return;
  }
  const TempDir root;
  const RangeServer srv = startChatServer(root.path, "ok", "");
  REQUIRE(srv.pid >= 0);

  SubtitleTranslator tr;
  SubtitleTranslateConfig cfg;
  cfg.endpoint = srv.base_url + "/v1";
  cfg.model = "test-model";
  tr.configure(cfg);

  const std::string kIn =
      "1\n00:00:01,000 --> 00:00:02,000\none\ntwo  spaces\n\n";
  std::string out, err;
  REQUIRE(tr.translate(kIn, "Spanish", &out, &err));

  const std::vector<ChatRequest> reqs = readChatRequests(root);
  REQUIRE(reqs.size() == 1);
  CHECK(reqs[0].body.find("1. one two spaces") != std::string::npos);

  const std::vector<SubtitleCue> cues = parseSubtitleText(out, SubtitleFormat::SubRip);
  REQUIRE(cues.size() == 1);
  CHECK(cues[0].text == "[tr] one two spaces");
#endif
}

TEST_CASE("translate_degrades_with_a_reason_on_every_failure") {
  // The all-or-nothing contract: whatever breaks, the answer is false, a
  // reason, and an untouched output. (The translate call is its own
  // statement: argument evaluation order would make reading err/out
  // inside the same call a race on fresh locals.)
  const std::string kIn = "1\n00:00:01,000 --> 00:00:02,000\nHello.\n\n";
  const auto fails = [](bool ok, const std::string& err, const std::string& out) {
    CHECK_FALSE(ok);
    CHECK(!err.empty());
    CHECK(out.empty());
  };

  SUBCASE("unconfigured endpoint never dials") {
    SubtitleTranslator tr;  // default config: everything empty
    std::string out, err;
    const bool ok = tr.translate(kIn, "Japanese", &out, &err);
    fails(ok, err, out);
  }
  SUBCASE("an empty model is unconfigured too") {
    SubtitleTranslator tr;
    SubtitleTranslateConfig cfg;
    cfg.endpoint = "http://127.0.0.1:1/v1";  // nothing listens; must not dial
    tr.configure(cfg);
    std::string out, err;
    const bool ok = tr.translate(kIn, "Japanese", &out, &err);
    fails(ok, err, out);
    CHECK(err.find("not configured") != std::string::npos);
  }
  SUBCASE("https endpoints degrade (no TLS client here)") {
    SubtitleTranslator tr;
    SubtitleTranslateConfig cfg;
    cfg.endpoint = "https://example.invalid/v1";
    cfg.model = "test-model";
    tr.configure(cfg);
    std::string out, err;
    const bool ok = tr.translate(kIn, "Japanese", &out, &err);
    fails(ok, err, out);
    CHECK(err.find("http://") != std::string::npos);
  }
  SUBCASE("no target language is refused before any dial") {
    SubtitleTranslator tr;
    SubtitleTranslateConfig cfg;
    cfg.endpoint = "http://127.0.0.1:1/v1";
    cfg.model = "test-model";
    tr.configure(cfg);
    std::string out, err;
    const bool ok = tr.translate(kIn, "", &out, &err);
    fails(ok, err, out);
  }
  SUBCASE("input without timestamps is not a subtitle") {
    SubtitleTranslator tr;
    SubtitleTranslateConfig cfg;
    cfg.endpoint = "http://127.0.0.1:1/v1";  // must be refused pre-dial
    cfg.model = "test-model";
    tr.configure(cfg);
    std::string out, err;
    const bool ok =
        tr.translate("plain prose with no cues at all", "Japanese", &out, &err);
    fails(ok, err, out);
    CHECK(err.find("not SubRip or WebVTT") != std::string::npos);
  }
  SUBCASE("a format-happy but cue-less input fails too") {
    SubtitleTranslator tr;
    SubtitleTranslateConfig cfg;
    cfg.endpoint = "http://127.0.0.1:1/v1";
    cfg.model = "test-model";
    tr.configure(cfg);
    std::string out, err;
    const bool ok = tr.translate("WEBVTT\n\n", "Japanese", &out, &err);
    fails(ok, err, out);
    CHECK(err.find("no cues") != std::string::npos);
  }
  SUBCASE("an endpoint with no host is not a usable url") {
    SubtitleTranslator tr;
    SubtitleTranslateConfig cfg;
    cfg.endpoint = "http://";  // scheme only: parse must refuse pre-dial
    cfg.model = "test-model";
    tr.configure(cfg);
    std::string out, err;
    const bool ok = tr.translate(kIn, "Japanese", &out, &err);
    fails(ok, err, out);
    CHECK(err.find("not a usable http:// url") != std::string::npos);
  }

#ifdef _WIN32
  MESSAGE("POSIX-only fixture cases; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping translator tests");
    return;
  }
  const TempDir root;  // the fixture servers record their requests here
  const auto server = [&root](const std::string& mode,
                              const std::string& key = "") {
    return startChatServer(root.path, mode, key);
  };
  const auto cfg_for = [](const RangeServer& s) {
    SubtitleTranslateConfig cfg;
    cfg.endpoint = s.base_url + "/v1";
    cfg.model = "test-model";
    cfg.timeout = 5000ms;
    return cfg;
  };

  SUBCASE("an unreachable endpoint is a false, not a throw") {
    SubtitleTranslator tr;
    SubtitleTranslateConfig cfg;
    cfg.endpoint = "http://127.0.0.1:1/v1";  // port 1: nothing listens
    cfg.model = "test-model";
    cfg.timeout = 2000ms;
    tr.configure(cfg);
    std::string out, err;
    const bool ok = tr.translate(kIn, "Japanese", &out, &err);
    fails(ok, err, out);
  }

  const std::string modes[] = {
      "status500", "badjson",  "nocontent", "emptychoices", "shortlines",
      "badnum",    "biglen",   "garbage",   "contentnum",   "badhex",
      "badescape", "shortu",   "trailbs",   "unterm",       "badsep",
      "onlydigits", "status100"};
  for (const std::string& mode : modes) {
    SUBCASE(mode.c_str()) {
      const RangeServer srv = server(mode);
      REQUIRE(srv.pid >= 0);
      SubtitleTranslator tr;
      tr.configure(cfg_for(srv));
      std::string out, err;
      const bool ok = tr.translate(kIn, "Japanese", &out, &err);
      fails(ok, err, out);
    }
  }

  SUBCASE("a wrong key is a 401 and a failed translation") {
    const RangeServer srv = server("ok", "sekret-key");
    REQUIRE(srv.pid >= 0);
    SubtitleTranslator tr;
    SubtitleTranslateConfig cfg = cfg_for(srv);
    cfg.api_key = "wrong-key";
    tr.configure(cfg);
    std::string out, err;
    const bool ok = tr.translate(kIn, "Japanese", &out, &err);
    fails(ok, err, out);
  }

  SUBCASE("a clean close mid-body is a failed translation") {
    const RangeServer srv = server("shortbody");
    REQUIRE(srv.pid >= 0);
    SubtitleTranslator tr;
    tr.configure(cfg_for(srv));
    std::string out, err;
    const bool ok = tr.translate(kIn, "Japanese", &out, &err);
    fails(ok, err, out);
    CHECK(err.find("mid-body") != std::string::npos);
  }

  SUBCASE("a stalling endpoint is cut off by the timeout") {
    const RangeServer srv = server("stall");
    REQUIRE(srv.pid >= 0);
    SubtitleTranslator tr;
    SubtitleTranslateConfig cfg = cfg_for(srv);
    cfg.timeout = 300ms;  // the socket timeout ends the wait, not patience
    tr.configure(cfg);
    std::string out, err;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = tr.translate(kIn, "Japanese", &out, &err);
    fails(ok, err, out);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10));
  }
#endif
}

TEST_CASE("translate_survives_json_escapes_in_both_directions") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping translator tests");
    return;
  }
  // Outgoing: a quote and a backslash in a cue must reach the server
  // escaped; UTF-8 rides raw. Incoming: the fixture answers with
  // ensure_ascii=True, so CJK and emoji come back as \uXXXX (surrogate
  // pairs included) and the decoder must rebuild the same text.
  const TempDir root;
  const RangeServer srv = startChatServer(root.path, "asciiesc", "");
  REQUIRE(srv.pid >= 0);

  SubtitleTranslator tr;
  SubtitleTranslateConfig cfg;
  cfg.endpoint = srv.base_url + "/v1";
  cfg.model = "test-model";
  tr.configure(cfg);

  const std::string kQuote = "Say \"hi\" \\ ok";
  const std::string kCjk = "\xE5\xAD\x97\xE5\xB9\x95";  // 字幕
  const std::string kEmoji = "hi \xF0\x9F\x98\x80";     // hi 😀
  const std::string kIn =
      "1\n00:00:01,000 --> 00:00:02,000\n" + kQuote + "\n\n"
      "2\n00:00:02,000 --> 00:00:03,000\n" + kCjk + "\n\n"
      "3\n00:00:03,000 --> 00:00:04,000\n" + kEmoji + "\n\n";
  std::string out, err;
  REQUIRE(tr.translate(kIn, "Japanese", &out, &err));

  const std::vector<ChatRequest> reqs = readChatRequests(root);
  REQUIRE(reqs.size() == 1);
  // \" and \\ on the wire where the cue said "hi" \
  CHECK(reqs[0].body.find("Say \\\"hi\\\" \\\\ ok") != std::string::npos);
  CHECK(reqs[0].body.find(kCjk) != std::string::npos);  // raw UTF-8 out

  const std::vector<SubtitleCue> cues = parseSubtitleText(out, SubtitleFormat::SubRip);
  REQUIRE(cues.size() == 3);
  CHECK(cues[0].text == "[tr] " + kQuote);
  CHECK(cues[1].text == "[tr] " + kCjk);
  CHECK(cues[2].text == "[tr] " + kEmoji);
#endif
}

TEST_CASE("translate_decodes_lone_surrogates_as_replacement_characters") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping translator tests");
    return;
  }
  // A lone surrogate (never valid UTF-8, but a hostile or broken endpoint
  // can still emit one) must come back as U+FFFD, not as invalid UTF-8 of
  // a surrogate value.
  const TempDir root;
  const RangeServer srv = startChatServer(root.path, "lonesur", "");
  REQUIRE(srv.pid >= 0);

  SubtitleTranslator tr;
  SubtitleTranslateConfig cfg;
  cfg.endpoint = srv.base_url + "/v1";
  cfg.model = "test-model";
  tr.configure(cfg);

  const std::string kIn = "1\n00:00:01,000 --> 00:00:02,000\nHello.\n\n";
  std::string out, err;
  REQUIRE(tr.translate(kIn, "Japanese", &out, &err));

  const std::vector<SubtitleCue> cues = parseSubtitleText(out, SubtitleFormat::SubRip);
  REQUIRE(cues.size() == 1);
  CHECK(cues[0].text ==
        "[tr] A\xEF\xBF\xBD" "\xC3\xA9" "B\xEF\xBF\xBD" "C");
#endif
}

TEST_CASE("translate_reply_without_content_length_is_read_to_eof") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping translator tests");
    return;
  }
  // A 200 with no Content-Length and Connection: close: the body is
  // whatever arrives before the close — still a perfectly good
  // translation, same as the download provider's read-until-EOF path.
  const TempDir root;
  const RangeServer srv = startChatServer(root.path, "nolength", "");
  REQUIRE(srv.pid >= 0);

  SubtitleTranslator tr;
  SubtitleTranslateConfig cfg;
  cfg.endpoint = srv.base_url + "/v1";
  cfg.model = "test-model";
  tr.configure(cfg);

  std::string out, err;
  const std::string kIn = "1\n00:00:01,000 --> 00:00:02,000\nSolo.\n\n";
  REQUIRE(tr.translate(kIn, "Italian", &out, &err));
  const std::vector<SubtitleCue> cues = parseSubtitleText(out, SubtitleFormat::SubRip);
  REQUIRE(cues.size() == 1);
  CHECK(cues[0].text == "[tr] Solo");
#endif
}

TEST_CASE("translate_decodes_the_full_json_escape_vocabulary") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping translator tests");
    return;
  }
  // The fixture's hand-built bodies put single-backslash escape sequences
  // on the wire (json.dumps would re-escape them). The client's string
  // reader must reconstruct the exact text behind each one — and the
  // reply parser must take a tab where it usually sees a space.
  const std::string kIn = "1\n00:00:01,000 --> 00:00:02,000\nSolo.\n\n";

  SUBCASE("uppercase hex, a tab after the number, and a 3-byte code point") {
    const TempDir root;
    const RangeServer srv = startChatServer(root.path, "hexmix", "");
    REQUIRE(srv.pid >= 0);
    SubtitleTranslator tr;
    SubtitleTranslateConfig cfg;
    cfg.endpoint = srv.base_url + "/v1";
    cfg.model = "test-model";
    tr.configure(cfg);
    std::string out, err;
    REQUIRE(tr.translate(kIn, "Japanese", &out, &err));
    const std::vector<SubtitleCue> cues = parseSubtitleText(out, SubtitleFormat::SubRip);
    REQUIRE(cues.size() == 1);
    CHECK(cues[0].text == "A\xC3\xA9\xE4\xBD\xA0");
  }

  SUBCASE("every simple escape decodes to its control byte") {
    const TempDir root;
    const RangeServer srv = startChatServer(root.path, "escmix", "");
    REQUIRE(srv.pid >= 0);
    SubtitleTranslator tr;
    SubtitleTranslateConfig cfg;
    cfg.endpoint = srv.base_url + "/v1";
    cfg.model = "test-model";
    tr.configure(cfg);
    std::string out, err;
    REQUIRE(tr.translate(kIn, "Japanese", &out, &err));
    // Asserted on the serialized file, not a re-parse: the parser reads a
    // bare CR as a line break, and this text deliberately carries one.
    // The fixture line is "1. a\/b\bc\fc\rd\te" — escape letters and
    // content letters interleave, so the decode reads a/b, backspace, c,
    // form feed, c, carriage return, d, tab, e.
    CHECK(out.find("\na/b\bc" "\x0c" "c\rd\te\n\n") != std::string::npos);
  }

  SUBCASE("a negative Content-Length reads to EOF and succeeds") {
    const TempDir root;
    const RangeServer srv = startChatServer(root.path, "neglen", "");
    REQUIRE(srv.pid >= 0);
    SubtitleTranslator tr;
    SubtitleTranslateConfig cfg;
    cfg.endpoint = srv.base_url + "/v1";
    cfg.model = "test-model";
    tr.configure(cfg);
    std::string out, err;
    REQUIRE(tr.translate(kIn, "Italian", &out, &err));
    const std::vector<SubtitleCue> cues = parseSubtitleText(out, SubtitleFormat::SubRip);
    REQUIRE(cues.size() == 1);
    CHECK(cues[0].text == "[tr] Solo");
  }
#endif
}

TEST_CASE("translate_escapes_control_bytes_into_the_json_body") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping translator tests");
    return;
  }
  // The wire writer's side of the vocabulary: a model name and cue text
  // carrying bytes the JSON grammar cannot ship raw (quote, tab, CR, a
  // C0 control) must reach the endpoint escaped — and a cue with a
  // control byte survives the round trip byte for byte.
  const TempDir root;
  const RangeServer srv = startChatServer(root.path, "ok", "");
  REQUIRE(srv.pid >= 0);

  SubtitleTranslator tr;
  SubtitleTranslateConfig cfg;
  cfg.endpoint = srv.base_url + "/v1";
  cfg.model = "mo\"dle\t\r";
  tr.configure(cfg);

  const std::string kIn =
      "1\n00:00:01,000 --> 00:00:02,000\nHi \x01there.\n\n";
  std::string out, err;
  REQUIRE(tr.translate(kIn, "Japanese", &out, &err));

  const std::vector<ChatRequest> reqs = readChatRequests(root);
  REQUIRE(reqs.size() == 1);
  CHECK(reqs[0].body.find("\"model\":\"mo\\\"dle\\t\\r\"") != std::string::npos);
  CHECK(reqs[0].body.find("Hi \\u0001there.") != std::string::npos);

  const std::vector<SubtitleCue> cues = parseSubtitleText(out, SubtitleFormat::SubRip);
  REQUIRE(cues.size() == 1);
  CHECK(cues[0].text == "[tr] Hi \x01there.");
#endif
}

TEST_CASE("translate_tolerates_null_out_and_err") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping translator tests");
    return;
  }
  // Callers that only care whether it worked may pass nullptrs; both
  // answers (and the refusal of an unconfigured translator) must come
  // back without a write through the null.
  const TempDir root;
  const RangeServer srv = startChatServer(root.path, "ok", "");
  REQUIRE(srv.pid >= 0);

  SubtitleTranslator tr;
  SubtitleTranslateConfig cfg;
  cfg.endpoint = srv.base_url + "/v1";
  cfg.model = "test-model";
  tr.configure(cfg);

  const std::string kIn = "1\n00:00:01,000 --> 00:00:02,000\nSolo.\n\n";
  REQUIRE(tr.translate(kIn, "Italian", nullptr, nullptr));

  SubtitleTranslator unconfigured;
  CHECK_FALSE(unconfigured.translate(kIn, "Italian", nullptr, nullptr));
#endif
}

TEST_CASE("translate_accepts_every_documented_reply_separator") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping translator tests");
    return;
  }
  // The header pins the contract: a reply line is "12.", "12)" or "12:"
  // — endpoints pick their own punctuation, and all three must map back.
  const TempDir root;
  const RangeServer srv = startChatServer(root.path, "sepvar", "");
  REQUIRE(srv.pid >= 0);

  SubtitleTranslator tr;
  SubtitleTranslateConfig cfg;
  cfg.endpoint = srv.base_url + "/v1";
  cfg.model = "test-model";
  tr.configure(cfg);

  const std::string kIn =
      "1\n00:00:01,000 --> 00:00:02,000\nFirst.\n\n"
      "2\n00:00:03,000 --> 00:00:04,000\nSecond.\n\n";
  std::string out, err;
  REQUIRE(tr.translate(kIn, "Japanese", &out, &err));
  const std::vector<SubtitleCue> cues = parseSubtitleText(out, SubtitleFormat::SubRip);
  REQUIRE(cues.size() == 2);
  CHECK(cues[0].text == "[tr] Alpha");
  CHECK(cues[1].text == "[tr] Beta");
  CHECK(cues[0].begin == 1000ms);
  CHECK(cues[1].end == 4000ms);
#endif
}

TEST_CASE("translate_endpoint_forms_and_batch_guard") {
#ifdef _WIN32
  MESSAGE("POSIX-only test; skipping");
  return;
#else
  if (std::system("command -v python3 >/dev/null 2>&1") != 0) {
    MESSAGE("python3 not available; skipping translator tests");
    return;
  }
  const TempDir root;
  const RangeServer srv = startChatServer(root.path, "ok", "");
  REQUIRE(srv.pid >= 0);
  const std::string kIn = "1\n00:00:01,000 --> 00:00:02,000\nSolo.\n\n";

  SUBCASE("a trailing slash on the endpoint") {
    SubtitleTranslator tr;
    SubtitleTranslateConfig cfg;
    cfg.endpoint = srv.base_url + "/v1/";
    cfg.model = "test-model";
    tr.configure(cfg);
    std::string out, err;
    REQUIRE(tr.translate(kIn, "Italian", &out, &err));
    const std::vector<ChatRequest> reqs = readChatRequests(root);
    REQUIRE(reqs.size() == 1);
    CHECK(reqs[0].path == "/v1/chat/completions");
  }

  SUBCASE("an endpoint that already spells the full path") {
    SubtitleTranslator tr;
    SubtitleTranslateConfig cfg;
    cfg.endpoint = srv.base_url + "/chat/completions";
    cfg.model = "test-model";
    tr.configure(cfg);
    std::string out, err;
    REQUIRE(tr.translate(kIn, "Italian", &out, &err));
    const std::vector<ChatRequest> reqs = readChatRequests(root);
    REQUIRE(reqs.size() == 1);
    CHECK(reqs[0].path == "/chat/completions");
  }

  SUBCASE("batch_cues below 1 is treated as 1") {
    // The inline-constructor form, and the guard that keeps a zero batch
    // from becoming an endless loop: three cues, three requests.
    SubtitleTranslateConfig cfg;
    cfg.endpoint = srv.base_url + "/v1";
    cfg.model = "test-model";
    cfg.batch_cues = 0;
    const SubtitleTranslator tr{cfg};
    std::string out, err;
    const std::string kThree =
        "1\n00:00:01,000 --> 00:00:02,000\nA\n\n"
        "2\n00:00:02,000 --> 00:00:03,000\nB\n\n"
        "3\n00:00:03,000 --> 00:00:04,000\nC\n\n";
    REQUIRE(tr.translate(kThree, "Italian", &out, &err));
    CHECK(readChatRequests(root).size() == 3);
  }
#endif
}

} // namespace
