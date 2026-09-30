#pragma once

// ASS Dialogue-line utilities for the FFmpeg feeding path (docs/mvp.md §6,
// style-faithful ASS playback). libavcodec's ASS decoder hands subtitle
// payloads back in the ff_ass_get_dialog form —
// "readorder,layer,style,name,mL,mR,mV,effect,text" — with no "Dialogue:"
// prefix and without start/end times: the timing rides on the AVSubtitle
// and the packet duration (Matroska BlockDuration), not on the payload.
// libass's ass_process_data() only accepts the complete 10-field
// "Dialogue:" line with ASS timestamps, so the FFmpegBackend rebuilds one
// per subtitle rect. These helpers are that rebuild, factored out as pure
// functions so the tolerant-parsing arms are unit-testable headlessly —
// through the real pipeline they only ever see the shapes the installed
// decoder happens to emit, which is exactly what a test cannot control.

#include "soar/core/subtitle_text.h"

#include <chrono>
#include <string>
#include <vector>

namespace soar {

// ASS timestamp from milliseconds: "h:mm:ss.cc" (centiseconds), the form a
// Dialogue line's Start/End fields carry. Negative input clamps to
// 0:00:00.00.
std::string assTimestamp(std::chrono::milliseconds ms);

// Rebuilds the full "Dialogue:" line from a decoder payload and the
// packet's presentation time and duration. The payload's layer, style,
// name, margins and effect are kept verbatim; so is the trailing text,
// which may itself contain commas (only the first 8 commas split fields —
// the text is field 10). A literal newline inside the payload would end
// the Dialogue line early, so it is respelled as the ASS hard break \N.
//
// Tolerant arms: a payload already carrying the "Dialogue:" prefix passes
// through unchanged (verbatim feeding — another decoder version's shape),
// surrounding brackets are stripped (ff_ass_get_dialog wraps some
// synthesized payloads), and anything that is not the 8-comma form is
// returned as-is rather than mangled.
std::string assDialogueLine(const char* ass, std::chrono::milliseconds pts,
                            std::chrono::milliseconds duration);

// Extracts plain timed cues from a complete ASS document (external
// .ass/.ssa sidecars): every "Dialogue:" line becomes a SubtitleCue with
// the timing from its Start/End fields and its text stripped of override
// blocks ({\pos(...)}, {\i1}, ...) — the same rule the plain-text pipeline
// follows, since those braces are directives, not content. \N and \n hard
// breaks become '\n' and \h becomes a space, so the cue reads like the
// same line would in SubRip; other backslash sequences pass through.
//
// This is the no-libass degrade for external documents (with libass the
// document goes to AssRenderer::loadDocument whole and this is unused) and
// the headless-testable parsing arm for both modes' file validation.
//
// Tolerant, like every parser here: lines that are not 10-field Dialogue
// records, fields whose timestamps do not parse, and cues whose text ends
// up empty are skipped; a cue whose End is missing-or-before-its-Start is
// kept with the default 2 s display time (kDefaultCueDuration), matching
// parseSubtitleText. Nothing throws.
std::vector<SubtitleCue> assDocumentCues(const std::string& content);

// Rebuilds a "Dialogue:" line for a plain-text cue the decoder handed back
// without any ASS wrapper — the raw SUBTITLE_TEXT rect shape. Not every
// text decoder produces that shape: FFmpeg 8's mov_text wraps its cues in
// ASS rects (measured — the verbatim-ass feed arm serves those on the
// canvas), so this serves the decoders/versions that do emit raw text.
// The event goes onto a streaming track armed with
// a synthesized default header (synthesizeAssDocument with no cues), whose
// "Default" style this line names; libass falls back to style 0 when the
// name does not resolve, so the line renders either way. Text bytes are
// kept except line breaks, which respell as \N exactly like
// synthesizeAssDocument's cue wrapping.
std::string assDialogueLineFromText(const std::string& text,
                                    std::chrono::milliseconds start,
                                    std::chrono::milliseconds end);

// Wraps plain cues in a default-styled ASS script: what an external
// SRT/WebVTT sidecar is rendered through when the build has libass
// (docs/mvp.md §6, batch 1c) — one synthesized document per selected
// sidecar, loaded exactly like a hand-authored one, so the canvas (and
// nothing else) draws the text. The style is ours by design: white with a
// black outline, bottom-center, font size scaled to the play resolution
// (the classic 20 px at 288p, 77 px at 1080p); glyph rendering itself is
// libass's, including its default-font fallback when the script's family
// resolves to nothing. PlayRes falls back to ASS's own 384x288 when the
// caller cannot size the video.
//
// Cue text keeps its bytes except line breaks, which respell as the \N
// hard break so one Dialogue line stays one cue; braces would read as
// override blocks to libass — the same lens assDocumentCues applies in
// reverse, and a rare thing in timed text. A cue with a missing or
// inverted End keeps the parser's 2 s default, matching every other path.
std::string synthesizeAssDocument(const std::vector<SubtitleCue>& cues,
                                  int play_res_x, int play_res_y);

} // namespace soar
