// Unit tests for the external-subtitle core (docs/mvp.md §6): the SRT /
// WebVTT text parser (src/core/subtitle_text.*) and the pluggable
// SubtitleProvider with its local sidecar implementation
// (src/core/subtitle_provider.*).
//
// Both are pure logic over strings plus one temp directory, so the suite
// needs no display, no media fixture and no network.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "soar/core/subtitle_provider.h"
#include "soar/core/subtitle_text.h"

#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef _WIN32
#  include <fcntl.h>
#  include <sys/resource.h>
#  include <unistd.h>
#endif

using namespace std::chrono_literals;
using soar::detectSubtitleFormat;
using soar::kDefaultCueDuration;
using soar::MediaSource;
using soar::parseSubtitleText;
using soar::readSubtitleFile;
using soar::SidecarSubtitleProvider;
using soar::SubtitleCandidate;
using soar::SubtitleCue;
using soar::SubtitleFormat;

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
  SUBCASE("unsupported") {
    // Bitmap / styled formats need a decoder, not this parser.
    CHECK(soar::subtitleFormatFromPath("movie.ass") == SubtitleFormat::Unknown);
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
  tmp.write("movie.ass", "styled subs this parser does not read");
  tmp.write("movie.srt.bak", "not a subtitle extension");
  tmp.write("moviey.srt", "a different movie whose name merely starts the same");
  tmp.write("other.srt", "unrelated");
  tmp.write("srt", "no stem");
  tmp.mkdir("movie.dir.srt");  // a directory that looks like a sidecar

  const SidecarSubtitleProvider provider;
  CHECK(provider.findCandidates(MediaSource{tmp.file("movie.mkv"), {}}).empty());
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

} // namespace
