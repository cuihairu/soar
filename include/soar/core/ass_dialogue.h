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

#include <chrono>
#include <string>

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

} // namespace soar
