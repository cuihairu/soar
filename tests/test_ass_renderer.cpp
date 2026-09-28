// Unit tests for AssRenderer (docs/mvp.md §6, style-faithful ASS/SSA
// playback): document loading, the streaming feed used for embedded
// Matroska tracks, event flushing on rewind, change detection, and the
// straight-alpha RGBA compositing contract.
//
// Rendering is deterministic by construction: libass runs with
// ASS_FONTPROVIDER_NONE and resolves glyphs from the checked-in fixture
// font (SOAR_TEST_FONT_FILE, tests/fixtures/NotoMono-Regular.ttf) or from
// bytes registered via addFont — no fontconfig, no system font sweep.
// The test script uses Outline=0/Shadow=0 with an opaque red fill, so
// every composited pixel is exactly (255, 0, 0, coverage) — the color
// assertions below would catch a straight/premultiplied mixup, a
// 0xAABBGGRR vs 0xRRGGBBAA channel swap, or an inverted alpha.

#ifdef SOAR_WITH_LIBASS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "soar/core/ass_renderer.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using soar::AssFrame;
using soar::AssRenderer;

namespace {

#ifndef SOAR_TEST_FONT_FILE
#  error "SOAR_TEST_FONT_FILE must point at tests/fixtures/NotoMono-Regular.ttf"
#endif

std::vector<std::uint8_t> readFileBytes(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  REQUIRE_MESSAGE(f.good(), "fixture font missing: " << path);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f),
                                   std::istreambuf_iterator<char>());
}

// A minimal but fully-formed ASS script: 160x120 PlayRes (the render
// tests use the same frame size, so glyph geometry is 1:1), one "Red"
// style (opaque pure-red fill, no outline, no shadow, no bold), and the
// given Dialogue line(s) (may be empty for an event-less document).
std::string makeAssDoc(const std::string& fontName, const char* dialogue) {
  return
      "[Script Info]\n"
      "; soar unit-test script\n"
      "ScriptType: v4.00+\n"
      "PlayResX: 160\n"
      "PlayResY: 120\n"
      "WrapStyle: 0\n"
      "ScaledBorderAndShadow: yes\n"
      "\n"
      "[V4+ Styles]\n"
      "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
      "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, "
      "ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
      "Alignment, MarginL, MarginR, MarginV, Encoding\n"
      "Style: Red," + fontName +
      ",24,&H000000FF,&H00FFFFFF,&H00000000,&H00000000,"
      "0,0,0,0,100,100,0,0,1,0,0,2,10,10,10,1\n"
      "\n"
      "[Events]\n"
      "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, "
      "Effect, Text\n" +
      std::string(dialogue);
}

const char* kHelloDialogue =
    "Dialogue: 0,0:00:00.00,0:00:01.00,Red,,0,0,0,,Hello\n";
const char* kSecondDialogue =
    "Dialogue: 0,0:00:01.00,0:00:02.00,Red,,0,0,0,,Second\n";

// The CodecPrivate of a Matroska ASS track: everything up to and
// including the [Events] Format line; the Dialogue lines then arrive as
// playback reaches them (feedEvent).
std::string makeCodecPrivate(const std::string& fontName) {
  std::string doc = makeAssDoc(fontName, "");
  const std::string kEventsFormat =
      "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, "
      "Effect, Text\n";
  const auto pos = doc.find(kEventsFormat);
  REQUIRE_MESSAGE(pos != std::string::npos, "doc generator is malformed");
  return doc.substr(0, pos + kEventsFormat.size());
}

void loadHelloDoc(AssRenderer& r) {
  REQUIRE(r.available());
  const std::string doc = makeAssDoc("Noto Mono", kHelloDialogue);
  CHECK(r.loadDocument(doc.data(), doc.size()));
  r.setDefaultFont(SOAR_TEST_FONT_FILE);
  r.setFrameSize(160, 120);
}

std::size_t countVisible(const AssFrame& f) {
  std::size_t n = 0;
  for (std::size_t i = 3; i < f.rgba.size(); i += 4) {
    if (f.rgba[i] > 0) ++n;
  }
  return n;
}

void checkAllPixelsRed(const AssFrame& f) {
  for (std::size_t i = 0; i + 3 < f.rgba.size(); i += 4) {
    if (f.rgba[i + 3] == 0) continue;  // fully transparent: no channel rule
    if (f.rgba[i + 0] != 255 || f.rgba[i + 1] != 0 || f.rgba[i + 2] != 0) {
      MESSAGE("non-red px idx=", i, " x=", (i / 4) % 160, " y=", (i / 4) / 160,
              " rgba=", (int)f.rgba[i], ",", (int)f.rgba[i + 1], ",",
              (int)f.rgba[i + 2], ",", (int)f.rgba[i + 3]);
    }
    CHECK(f.rgba[i + 0] == 255);       // R — the script's &H000000FF fill
    CHECK(f.rgba[i + 1] == 0);         // G
    CHECK(f.rgba[i + 2] == 0);         // B
  }
}

} // namespace

TEST_CASE("document render: styled text composites onto the RGBA canvas") {
  AssRenderer r;
  loadHelloDoc(r);

  AssFrame f;
  REQUIRE(r.renderAt(500, &f));
  CHECK(f.width == 160);
  CHECK(f.height == 120);
  CHECK(f.rgba.size() == static_cast<std::size_t>(160) * 120 * 4);
  // The glyph rasterized: real coverage, not a blank canvas.
  CHECK(countVisible(f) > 50);
  checkAllPixelsRed(f);
}

TEST_CASE("document render: PlayRes is scaled to the frame size") {
  AssRenderer r;
  loadHelloDoc(r);
  r.setFrameSize(320, 240);  // 2x PlayRes

  AssFrame f;
  REQUIRE(r.renderAt(500, &f));
  CHECK(f.width == 320);
  CHECK(f.height == 240);
  CHECK(countVisible(f) > 100);
  // A later resize moves the canvas back (and resets the baseline).
  r.setFrameSize(160, 120);
  REQUIRE(r.renderAt(500, &f));
  CHECK(f.width == 160);
  CHECK(f.height == 120);
}

TEST_CASE("time window: vanish is reported once as a change") {
  AssRenderer r;
  loadHelloDoc(r);

  AssFrame f;
  // Outside the event window, with no prior content: empty, no change.
  CHECK_FALSE(r.renderAt(1500, &f));
  CHECK(f.rgba.empty());
  CHECK(f.width == 160);
  CHECK(f.height == 120);
  CHECK_FALSE(f.changed);

  // Visible at 0.5 s.
  REQUIRE(r.renderAt(500, &f));
  CHECK_FALSE(f.rgba.empty());

  // After the event ends: the content is gone — that loss is a change,
  // exactly once.
  CHECK_FALSE(r.renderAt(1500, &f));
  CHECK(f.rgba.empty());
  CHECK(f.changed);
  CHECK_FALSE(r.renderAt(1500, &f));
  CHECK_FALSE(f.changed);
}

TEST_CASE("change detection: a stable frame does not re-flag") {
  AssRenderer r;
  loadHelloDoc(r);

  AssFrame f;
  REQUIRE(r.renderAt(500, &f));
  // Second render at the same timestamp produces an identical image
  // list — a texture upload can be skipped.
  REQUIRE(r.renderAt(500, &f));
  CHECK_FALSE(f.changed);
  // A different timestamp inside the same event also stays stable.
  REQUIRE(r.renderAt(700, &f));
  CHECK_FALSE(f.changed);
}

TEST_CASE("flushEvents drops accumulated events (rewind rescan)") {
  AssRenderer r;
  loadHelloDoc(r);

  AssFrame f;
  REQUIRE(r.renderAt(500, &f));
  CHECK_FALSE(f.rgba.empty());

  r.flushEvents();
  CHECK_FALSE(r.renderAt(500, &f));
  CHECK(f.rgba.empty());
  CHECK(f.changed);

  // Re-feeding after the flush (what the decoder does when it rescans
  // cues on a backwards seek) restores the render.
  CHECK(r.feedEvent(kHelloDialogue));
  REQUIRE(r.renderAt(500, &f));
  CHECK_FALSE(f.rgba.empty());
  checkAllPixelsRed(f);
}

TEST_CASE("streaming mode: CodecPrivate first, Dialogue lines after") {
  AssRenderer r;
  REQUIRE(r.available());

  // Without a stream started, the feed entry points are no-ops.
  CHECK_FALSE(r.feedEvent(kHelloDialogue));
  const std::string cp0 = makeCodecPrivate("Noto Mono");
  CHECK_FALSE(r.feedCodecPrivate(cp0.data(), cp0.size()));
  CHECK_FALSE(r.renderAt(0, nullptr));
  // A rejected render still clears a caller-supplied frame.
  AssFrame fresh;
  CHECK_FALSE(r.renderAt(500, &fresh));
  CHECK(fresh.rgba.empty());

  REQUIRE(r.startStream());
  r.setDefaultFont(SOAR_TEST_FONT_FILE);
  r.setFrameSize(160, 120);
  // Re-passing the live size is the UI's every-presentation path; it must
  // early-out without resetting the change baseline.
  r.setFrameSize(160, 120);

  const std::string cp = makeCodecPrivate("Noto Mono");
  CHECK(r.feedCodecPrivate(cp.data(), cp.size()));

  AssFrame f;
  // Styles are in, but no event has arrived yet.
  CHECK_FALSE(r.renderAt(500, &f));
  CHECK(f.rgba.empty());

  // Null inputs reject cleanly at each entry point once the stream is
  // live — every guard exists so the decoder can pass through junk
  // packets without a crash.
  CHECK_FALSE(r.feedEvent(nullptr));
  CHECK_FALSE(r.feedCodecPrivate(nullptr, 0));
  CHECK_FALSE(r.loadDocument(nullptr, 0));
  CHECK_FALSE(r.addFont("fallback", nullptr, 0));

  CHECK(r.feedEvent(kHelloDialogue));
  REQUIRE(r.renderAt(500, &f));
  CHECK_FALSE(f.rgba.empty());
  checkAllPixelsRed(f);

  // Same width, new height: the size test's second leg must see a
  // differing height and treat it as a real geometry change.
  r.setFrameSize(160, 90);
  REQUIRE(r.renderAt(500, &f));
  CHECK_FALSE(f.rgba.empty());

  // An empty line is not an event.
  CHECK_FALSE(r.feedEvent(""));
}

TEST_CASE("streaming rewind: flush, then the rescanned cues render again") {
  AssRenderer r;
  REQUIRE(r.available());
  REQUIRE(r.startStream());
  r.setDefaultFont(SOAR_TEST_FONT_FILE);
  r.setFrameSize(160, 120);
  const std::string cp = makeCodecPrivate("Noto Mono");
  CHECK(r.feedCodecPrivate(cp.data(), cp.size()));
  CHECK(r.feedEvent(kHelloDialogue));
  CHECK(r.feedEvent(kSecondDialogue));

  AssFrame f;
  REQUIRE(r.renderAt(1500, &f));  // inside the second event
  CHECK_FALSE(f.rgba.empty());

  // Seek back into the first event: the decoder rescans both cues, so
  // the embedder flushes and re-feeds. Without the flush the re-fed
  // first event would overlap its still-loaded copy as a duplicate.
  r.flushEvents();
  CHECK(r.feedEvent(kHelloDialogue));
  REQUIRE(r.renderAt(500, &f));
  CHECK_FALSE(f.rgba.empty());
  checkAllPixelsRed(f);
}

TEST_CASE("registered font resolves the family without a font file") {
  AssRenderer r;
  REQUIRE(r.available());
  const auto font = readFileBytes(SOAR_TEST_FONT_FILE);
  REQUIRE(font.size() > 1000);
  CHECK(r.addFont("Noto Mono", font.data(), font.size()));

  const std::string doc = makeAssDoc("Noto Mono", kHelloDialogue);
  CHECK(r.loadDocument(doc.data(), doc.size()));
  // Deliberately no setDefaultFont: the family must resolve from the
  // registered bytes alone (the Matroska-attachment path).
  r.setFrameSize(160, 120);

  AssFrame f;
  REQUIRE(r.renderAt(500, &f));
  CHECK(countVisible(f) > 50);
  checkAllPixelsRed(f);
}

TEST_CASE("argument guards and preconditions") {
  AssRenderer r;
  REQUIRE(r.available());

  CHECK_FALSE(r.loadDocument(nullptr, 10));
  const std::string doc = makeAssDoc("Noto Mono", kHelloDialogue);
  CHECK_FALSE(r.loadDocument(doc.data(), 0));

  const auto font = readFileBytes(SOAR_TEST_FONT_FILE);
  CHECK_FALSE(r.addFont("x", nullptr, 10));
  CHECK_FALSE(r.addFont(nullptr, font.data(), font.size()));
  CHECK_FALSE(r.addFont("x", font.data(), 0));

  AssFrame f;
  CHECK_FALSE(r.renderAt(0, nullptr));  // null out pointer
  CHECK_FALSE(r.renderAt(0, &f));       // no frame size configured yet
  CHECK(f.rgba.empty());
  r.setFrameSize(0, 0);                 // invalid: ignored, not a crash
  CHECK_FALSE(r.renderAt(0, &f));
  r.setFrameSize(160, 120);
  CHECK_FALSE(r.renderAt(0, &f));       // still no document loaded

  // Document loaded but the frame size was reset (fresh canvas, no
  // geometry): the render must bail rather than trust a zero-sized one.
  CHECK(r.loadDocument(doc.data(), doc.size()));
  AssFrame no_size;
  CHECK_FALSE(r.renderAt(0, &no_size));
  CHECK(no_size.rgba.empty());
}

TEST_CASE("fully transparent glyphs composite nothing but still change") {
  // {\alpha&HFF&} is ASS for "fully transparent": libass still emits the
  // glyph images, but every pixel's source alpha is 255 in libass's
  // inverted convention, so the compositor's alpha-inversion guard must
  // skip them all. The frame is reported changed (there ARE images) yet
  // not a single visible pixel may appear.
  AssRenderer r;
  REQUIRE(r.available());
  const std::string doc = makeAssDoc(
      "Noto Mono",
      "Dialogue: 0,0:00:00.00,0:00:01.00,Red,,0,0,0,,{\\alpha&HFF&}Ghost\n");
  CHECK(r.loadDocument(doc.data(), doc.size()));
  r.setDefaultFont(SOAR_TEST_FONT_FILE);
  r.setFrameSize(160, 120);
  AssFrame f;
  CHECK(r.renderAt(500, &f));
  CHECK(f.changed);
  CHECK(countVisible(f) == 0);
  CHECK(f.rgba.size() == static_cast<std::size_t>(160) * 120 * 4);
}

TEST_CASE("overlapping events composite onto each other") {
  // Two simultaneous events pinned to the same spot via \pos: the second
  // glyph's coverage blends over the first's. The pixel-color contract
  // (pure red) must survive the blend, and the union must be visible.
  AssRenderer r;
  REQUIRE(r.available());
  const std::string doc = makeAssDoc(
      "Noto Mono",
      "Dialogue: 0,0:00:00.00,0:00:02.00,Red,,0,0,0,,{\\pos(80,60)}Base\n"
      "Dialogue: 1,0:00:00.00,0:00:02.00,Red,,0,0,0,,{\\pos(80,60)}Over\n");
  CHECK(r.loadDocument(doc.data(), doc.size()));
  r.setDefaultFont(SOAR_TEST_FONT_FILE);
  r.setFrameSize(160, 120);
  AssFrame f;
  REQUIRE(r.renderAt(500, &f));
  CHECK(countVisible(f) > 50);
  checkAllPixelsRed(f);
  // Same instant again: nothing new to composite, not flagged again.
  AssFrame again;
  CHECK(r.renderAt(500, &again));
  CHECK_FALSE(again.changed);
}

TEST_CASE("pre-stream and pre-geometry calls stay safe no-ops") {
  // Every mutating/reading call made before startStream() (no track) or
  // before the first setFrameSize() (no canvas geometry) must refuse
  // cleanly. The UI hits exactly these orders when a frame presents
  // before the size handshake, and the decode thread can flush before
  // any event arrived.
  AssRenderer r;
  REQUIRE(r.available());

  // No track yet: feeds refuse, flush and render are no-ops.
  const std::string cp = makeCodecPrivate("Noto Mono");
  CHECK_FALSE(r.feedCodecPrivate(cp.data(), cp.size()));
  CHECK_FALSE(r.feedCodecPrivate(nullptr, 5));  // null payload arm
  CHECK_FALSE(r.feedEvent(kHelloDialogue));
  CHECK_FALSE(r.feedEvent(nullptr));            // null line arm
  r.flushEvents();                              // no-op, not a crash
  AssFrame f;
  CHECK_FALSE(r.renderAt(0, &f));               // no track, no geometry
  CHECK(f.rgba.empty());

  // Invalid geometry halves: (0, 0) exercises the width arm, a positive
  // width with zero height the height arm — neither may touch state.
  r.setFrameSize(160, 0);
  r.setFrameSize(0, 120);

  // Track started but the frame size handshake has not happened yet:
  // renderAt refuses with a reset, empty frame. An empty Dialogue line
  // fed now reaches the size guard inside feedEvent and refuses.
  CHECK(r.startStream());
  CHECK_FALSE(r.feedEvent(""));  // empty line arm
  AssFrame g;
  CHECK_FALSE(r.renderAt(10, &g));
  CHECK(g.rgba.empty());
  CHECK(g.width == 0);
  CHECK(g.height == 0);
  CHECK_FALSE(g.changed);
}

TEST_CASE("tolerant parsing: noise and event-less documents stay safe") {
  AssRenderer r;
  REQUIRE(r.available());

  // Not an ASS script: libass keeps what it understands (an empty
  // track) — either way nothing may render and nothing may crash.
  const char* junk = "this is not an ASS script\r\njust noise\r\n";
  if (r.loadDocument(junk, std::strlen(junk))) {
    r.setFrameSize(160, 120);
    AssFrame f;
    CHECK_FALSE(r.renderAt(0, &f));
    CHECK(f.rgba.empty());
  }

  // A well-formed document without events loads but never shows.
  const std::string doc = makeAssDoc("Noto Mono", "");
  CHECK(r.loadDocument(doc.data(), doc.size()));
  r.setDefaultFont(SOAR_TEST_FONT_FILE);
  r.setFrameSize(160, 120);
  AssFrame f;
  CHECK_FALSE(r.renderAt(500, &f));
  CHECK(f.rgba.empty());
}

#else // !SOAR_WITH_LIBASS

// Keep the test binary meaningful when libass is not compiled in.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

TEST_CASE("ASS renderer tests require SOAR_WITH_LIBASS") {
  MESSAGE("libass not compiled in; nothing to test here");
}

#endif

// ---------------------------------------------------------------------------
// Dialogue-line reassembly (soar/ass_dialogue.h) — pure functions shared by
// the FFmpeg feeding path, independent of libass, so they are tested in
// every configuration, stub included.
// ---------------------------------------------------------------------------

#include "soar/core/ass_dialogue.h"

#include <chrono>

using namespace std::chrono_literals;

TEST_CASE("assTimestamp spells h:mm:ss.cc and clamps negatives") {
  using soar::assTimestamp;
  CHECK(assTimestamp(0ms) == "0:00:00.00");
  CHECK(assTimestamp(500ms) == "0:00:00.50");
  CHECK(assTimestamp(999ms) == "0:00:00.99");
  CHECK(assTimestamp(1000ms) == "0:00:01.00");   // centisecond carry
  CHECK(assTimestamp(61500ms) == "0:01:01.50");  // minute + second carry
  CHECK(assTimestamp(3661500ms) == "1:01:01.50"); // hour digit stays bare
  CHECK(assTimestamp(-1ms) == "0:00:00.00");      // negative clamps, no '-'
}

TEST_CASE("assDialogueLine rebuilds the 10-field line from decoder payloads") {
  using soar::assDialogueLine;
  using R = std::string;

  // The ff_ass_get_dialog form: 8 commas, text last. Text may contain
  // commas — only the first 8 split fields.
  CHECK(assDialogueLine("0,0,Red,,0,0,0,,Hello, world", 1500ms, 250ms) ==
        R("Dialogue: 0,0:00:01.50,0:00:01.75,Red,,0,0,0,,Hello, world"));

  // Bracket-wrapped payloads lose the brackets, then rebuild normally.
  CHECK(assDialogueLine("[0,0,Red,,0,0,0,,Hi]", 0ms, 10ms) ==
        R("Dialogue: 0,0:00:00.00,0:00:00.01,Red,,0,0,0,,Hi"));

  // A payload already carrying the "Dialogue:" prefix passes through
  // verbatim — its own timestamps win over the packet's.
  CHECK(assDialogueLine("Dialogue: 0,0:00:05.00,0:00:06.00,Red,,0,0,0,,Orig",
                        1ms, 1ms) ==
        R("Dialogue: 0,0:00:05.00,0:00:06.00,Red,,0,0,0,,Orig"));

  // Fewer than 8 commas: not a shape we can rebuild — returned as-is
  // rather than mangled into a truncated Dialogue line.
  CHECK(assDialogueLine("0,1,Red", 0ms, 0ms) == R("0,1,Red"));

  // A literal newline would terminate the Dialogue line early; it must be
  // respelled as the ASS hard break \N.
  CHECK(assDialogueLine("0,0,Red,,0,0,0,,Two\nlines", 0ms, 0ms) ==
        R("Dialogue: 0,0:00:00.00,0:00:00.00,Red,,0,0,0,,Two\\Nlines"));

  // No payload at all: empty line, never a crash — nul pointer and the
  // empty string take the same road.
  CHECK(assDialogueLine(nullptr, 0ms, 0ms) == R());
  CHECK(assDialogueLine("", 0ms, 0ms) == R());
}
