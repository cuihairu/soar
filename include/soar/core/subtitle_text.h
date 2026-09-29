#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace soar {

// One timed caption, as produced by parseSubtitleText(). `text` keeps the
// source line breaks ('\n'), exactly like the FFmpeg SRT/WebVTT decoders
// hand ASS-style rects to the renderer, so both subtitle sources feed the
// UI the same way.
struct SubtitleCue {
  std::chrono::milliseconds begin{0};
  std::chrono::milliseconds end{0};
  std::string text;
};

// Subtitle container formats. SubRip/WebVtt are plain text; Ass is a full
// ASS/SSA script document (rendered style-faithfully through libass when
// the build has it, extracted to plain cues otherwise). Bitmap ones (PGS,
// VobSub, dvdsub) are deliberately absent: they need a decoder, not a
// parser, and the sidecar provider never offers them.
enum class SubtitleFormat {
  Unknown,
  SubRip,
  WebVtt,
  Ass
};

// Format implied by a file extension, case-insensitively: ".srt" -> SubRip,
// ".vtt"/".webvtt" -> WebVtt, ".ass"/".ssa" -> Ass, anything else (including
// no extension) -> Unknown.
SubtitleFormat subtitleFormatFromPath(const std::string& path);

// Format implied by the content: a "WEBVTT" first line decides it, else
// whichever marker shows up first — a "Dialogue:" line or a "[Script Info]"
// section header decides Ass, the first parseable "-->" block decides
// SubRip vs WebVtt by the fraction separator, since SubRip writes
// "00:00:01,000" and WebVTT "00:00:01.000". Unknown when the text is empty
// or carries none of the markers.
SubtitleFormat detectSubtitleFormat(const std::string& content);

// Parse SubRip or WebVTT text into cues, in file order.
//
// Deliberately forgiving, because subtitle files in the wild are: a UTF-8
// BOM (first line or a stray one mid-file), CRLF or bare-CR line endings, a
// missing hour field ("01:23,456"), a fraction of 1-6 digits, WebVTT cue
// identifiers and cue settings ("align:start position:0%"), NOTE/STYLE/
// REGION blocks, and header metadata after "WEBVTT".
//
// A block whose timestamp pair does not parse, or that carries no text, is
// skipped and parsing resumes with the next block — one bad cue never
// costs the rest of the file. A cue whose end is missing or precedes its
// begin is kept (the text is worth showing) and given a 2 s display time.
// Nothing throws; a hopeless file just returns an empty vector. A text line
// that itself contains "-->" ends the cue there and is re-examined as the
// start of the next block.
std::vector<SubtitleCue> parseSubtitleText(
    const std::string& content,
    SubtitleFormat format = SubtitleFormat::Unknown);

// How long a cue is shown when its file gives no usable end timestamp.
extern const std::chrono::milliseconds kDefaultCueDuration;

// Read a whole subtitle file into `out`. False (leaving `out` untouched)
// when the path is not an existing regular file — a missing sidecar is a
// normal outcome, not an error worth a message — and also when the file
// does exist but the OS refuses to open it (descriptor limit, permissions),
// which a stat() cannot see.
bool readSubtitleFile(const std::string& path, std::string& out);

} // namespace soar
