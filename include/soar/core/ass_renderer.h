#pragma once

// ASS subtitle rendering (docs/mvp.md §6, style-faithful playback): wraps
// libass and composites its glyph bitmaps into one straight-alpha RGBA8888
// canvas the embedder can upload as a texture. The libass headers stay in
// the .cc — this component is the only place that includes them, so the
// public surface stays buildable without the dependency.
//
// Two loading modes mirror the two track sources:
//   - loadDocument(): a complete .ass/.ssa script (external files) —
//     styles, header and events in one buffer, parsed in one shot.
//   - startStream() + feedCodecPrivate() + feedEvent(): an embedded
//     Matroska ASS track, whose CodecPrivate (styles) arrives first and
//     whose events then arrive as playback reaches them. Events are fed
//     with ass_process_data(), which appends without duplicate checking;
//     the embedder drives flushEvents() on rewind (a seek backwards makes
//     the decoder rescan cues it already fed, which would otherwise pile
//     up as duplicate overlapping events).
//
// Everything degrades to a no-op when the build has no libass: available()
// is false and every call returns false/does nothing. Callers must gate on
// available() and keep their plain-text path alive — that is the same
// optional-capability contract SDL2/FFmpeg/ImGui already follow.
//
// Thread-safe: every public method takes the internal lock, so the decode
// thread (feedEvent / flushEvents) and the UI thread (renderAt /
// setFrameSize) can share one renderer without external synchronization.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace soar {

// One rendered subtitle frame. RGBA8888, straight (non-premultiplied)
// alpha, fully transparent where nothing is drawn; w*h*4 bytes.
struct AssFrame {
  std::vector<std::uint8_t> rgba;
  int width = 0;
  int height = 0;
  // True when this render differs from the previous one (libass
  // detect_change). Texture upload can be skipped when false — the
  // canvas still holds the previous render's pixels.
  bool changed = false;
};

class AssRenderer {
 public:
  AssRenderer();
  ~AssRenderer();
  AssRenderer(const AssRenderer&) = delete;
  AssRenderer& operator=(const AssRenderer&) = delete;

  // False unless the build links libass and its renderer initialized.
  bool available() const;

  // Register an in-memory font under the given family name (TTF/OTF
  // bytes). Matroska font attachments and deterministic tests both go
  // through this — libass resolves the ASS script's Fontname against
  // registered fonts before falling back to the default font file.
  bool addFont(const char* name, const std::uint8_t* data, std::size_t size);

  // Explicit fallback font file (a real path). Optional: without it,
  // glyph lookups miss for families that were never added, and libass
  // renders nothing for them.
  void setDefaultFont(const std::string& path);

  // Parse a complete script document (external .ass/.ssa). Replaces any
  // previous track. False when the build has no libass or the buffer is
  // empty; a syntactically broken document still parses (libass keeps
  // what it understands), matching the tolerant-parser contract the
  // plain-text decoders follow.
  bool loadDocument(const char* data, std::size_t size);

  // Streaming mode (embedded tracks). startStream() swaps in a fresh
  // empty track; feedCodecPrivate() parses the Matroska CodecPrivate
  // section (script header + styles); feedEvent() appends one event from
  // a full "Dialogue: ..." line as the decoder emits it.
  bool startStream();
  bool feedCodecPrivate(const char* data, std::size_t size);
  bool feedEvent(const char* dialogue_line);
  // Drop all accumulated events (rewind: the decoder rescans the cues it
  // already fed, so without this they would overlap as duplicates).
  void flushEvents();

  // Map the ASS canvas onto a video resolution. The script's PlayRes
  // scaling is libass's job; this is the output size of renderAt(). A
  // size change invalidates the change-detection baseline (the next
  // render reports changed=true). Idempotent: re-setting the current
  // size keeps the baseline intact, so callers may pass the frame size
  // every presentation without churn.
  void setFrameSize(int w, int h);

  // Render at pts_ms into out. False when nothing is visible at that
  // time (out is cleared); otherwise out holds the composited canvas and
  // changed says whether it differs from the previous render.
  bool renderAt(std::int64_t pts_ms, AssFrame* out);

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

} // namespace soar
