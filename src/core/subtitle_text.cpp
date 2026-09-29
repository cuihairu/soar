#include "soar/core/subtitle_text.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace soar {

const std::chrono::milliseconds kDefaultCueDuration{2000};

namespace {

const char* const kBom = "\xEF\xBB\xBF";

// Space and tab only: every caller hands this a line that splitLines has
// already stripped of CR and LF, so the exotic blanks are unreachable here.
std::string trim(const std::string& s) {
  const auto space = [](char c) { return c == ' ' || c == '\t'; };
  std::size_t b = 0;
  std::size_t e = s.size();
  while (b < e && space(s[b])) {
    ++b;
  }
  while (e > b && space(s[e - 1])) {
    --e;
  }
  return s.substr(b, e - b);
}

bool isBlank(const std::string& s) {
  return trim(s).empty();
}

std::string stripBom(const std::string& s) {
  if (s.rfind(kBom, 0) == 0) {
    return s.substr(3);
  }
  return s;
}

std::string toLower(const std::string& s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

// \n, \r\n and a bare \r all end a line; a trailing newline at EOF does not
// add an empty line. BOMs are dropped per line, which covers both the file
// prologue and a stray one glued in by a bad concatenating editor.
std::vector<std::string> splitLines(const std::string& text) {
  std::vector<std::string> lines;
  std::string cur;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '\n') {
      lines.push_back(stripBom(cur));
      cur.clear();
    } else if (c == '\r') {
      lines.push_back(stripBom(cur));
      cur.clear();
      if (i + 1 < text.size() && text[i + 1] == '\n') {
        ++i;
      }
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) {
    lines.push_back(stripBom(cur));
  }
  return lines;
}

bool startsWithWord(const std::string& s, const char* word) {
  const std::size_t n = std::char_traits<char>::length(word);
  if (s.size() < n || s.compare(0, n, word) != 0) {
    return false;
  }
  // "NOTE" must not swallow a cue whose identifier happens to start with
  // those letters ("NOTES: draft"), but "NOTE this is a comment" and a bare
  // "STYLE" both count.
  return s.size() == n || s[n] == ' ' || s[n] == '\t';
}

// "WEBVTT" on its first line (an optional description may follow it).
bool isWebVttHeader(const std::string& line) {
  return startsWithWord(line, "WEBVTT");
}

// WebVTT blocks that are metadata, not cues: the header, NOTE comments,
// STYLE and REGION blocks. Each runs until the next blank line.
bool isWebVttMetadataBlock(const std::string& line) {
  return isWebVttHeader(line) || startsWithWord(line, "NOTE") ||
         startsWithWord(line, "STYLE") || startsWithWord(line, "REGION");
}

bool parseUnsigned(const std::string& s, long long& out) {
  if (s.empty()) {
    return false;
  }
  long long v = 0;
  for (const char c : s) {
    if (c < '0' || c > '9') {
      return false;
    }
    v = v * 10 + (c - '0');
    // A field this large is a mis-parsed line, not a real timestamp; bail
    // out before the arithmetic below can overflow.
    if (v > 1000000) {
      return false;
    }
  }
  out = v;
  return true;
}

// "HH:MM:SS,mmm" (SubRip), "HH:MM:SS.mmm" / "MM:SS.mmm" (WebVTT), hour
// field and fraction both optional. The fraction is right-padded to
// milliseconds ("5" -> 500 ms) and truncated past them ("1234" -> 123 ms).
bool parseTimestamp(const std::string& raw, std::chrono::milliseconds& out) {
  const std::string s = trim(raw);
  const std::size_t colon2 = s.rfind(':');
  if (colon2 == std::string::npos) {
    return false;
  }

  const std::string head = s.substr(0, colon2);
  const std::string sec_field = s.substr(colon2 + 1);
  const std::size_t colon1 = head.rfind(':');

  long long hours = 0;
  long long minutes = 0;
  long long seconds = 0;
  long long millis = 0;
  if (colon1 == std::string::npos) {
    if (!parseUnsigned(head, minutes)) {
      return false;
    }
  } else if (!parseUnsigned(head.substr(0, colon1), hours) ||
             !parseUnsigned(head.substr(colon1 + 1), minutes)) {
    return false;
  }

  const std::size_t sep = sec_field.find_first_of(",.");
  if (!parseUnsigned(sep == std::string::npos ? sec_field : sec_field.substr(0, sep), seconds)) {
    return false;
  }
  if (sep != std::string::npos) {
    std::string frac = sec_field.substr(sep + 1);
    if (frac.empty()) {
      return false;
    }
    for (const char c : frac) {
      if (c < '0' || c > '9') {
        return false;
      }
    }
    if (frac.size() > 3) {
      frac.resize(3);
    }
    while (frac.size() < 3) {
      frac += '0';
    }
    millis = (frac[0] - '0') * 100 + (frac[1] - '0') * 10 + (frac[2] - '0');
  }

  out = std::chrono::milliseconds(((hours * 60 + minutes) * 60 + seconds) * 1000 + millis);
  return true;
}

} // namespace

SubtitleFormat subtitleFormatFromPath(const std::string& path) {
  const std::size_t dot = path.rfind('.');
  if (dot == std::string::npos) {
    return SubtitleFormat::Unknown;
  }
  const std::string ext = toLower(path.substr(dot));
  if (ext == ".srt") {
    return SubtitleFormat::SubRip;
  }
  if (ext == ".vtt" || ext == ".webvtt") {
    return SubtitleFormat::WebVtt;
  }
  if (ext == ".ass" || ext == ".ssa") {
    return SubtitleFormat::Ass;
  }
  return SubtitleFormat::Unknown;
}

SubtitleFormat detectSubtitleFormat(const std::string& content) {
  if (content.empty()) {
    return SubtitleFormat::Unknown;
  }
  for (const std::string& line : splitLines(content)) {
    if (isBlank(line)) {
      continue;
    }
    if (isWebVttHeader(line)) {
      return SubtitleFormat::WebVtt;
    }
    break;
  }
  for (const std::string& line : splitLines(content)) {
    // ASS documents announce themselves with a script header section or
    // event lines; either marker wins as soon as it shows up. A timed
    // "-->" block always precedes a cue's text, so an SRT/VTT file whose
    // cue *text* quotes "Dialogue:" still resolves by its timestamps.
    if (line.rfind("[Script Info]", 0) == 0 || line.rfind("Dialogue:", 0) == 0) {
      return SubtitleFormat::Ass;
    }
    const std::size_t arrow = line.find("-->");
    if (arrow == std::string::npos) {
      continue;
    }
    std::chrono::milliseconds probe{0};
    const std::string head = line.substr(0, arrow);
    if (!parseTimestamp(head, probe)) {
      continue;
    }
    return head.find(',') != std::string::npos ? SubtitleFormat::SubRip
                                                : SubtitleFormat::WebVtt;
  }
  return SubtitleFormat::Unknown;
}

std::vector<SubtitleCue> parseSubtitleText(const std::string& content, SubtitleFormat format) {
  std::vector<SubtitleCue> cues;
  if (content.empty()) {
    return cues;
  }
  const SubtitleFormat fmt =
      format == SubtitleFormat::Unknown ? detectSubtitleFormat(content) : format;
  if (fmt == SubtitleFormat::Unknown) {
    return cues;
  }

  const std::vector<std::string> lines = splitLines(content);
  std::size_t i = 0;
  while (i < lines.size()) {
    if (isBlank(lines[i])) {
      ++i;
      continue;
    }
    if (fmt == SubtitleFormat::WebVtt && isWebVttMetadataBlock(lines[i])) {
      while (i < lines.size() && !isBlank(lines[i])) {
        ++i;
      }
      continue;
    }

    // A cue starts at its timestamp line, or one line below it when that
    // line is a SubRip index / WebVTT cue identifier.
    std::size_t ts = i;
    std::size_t arrow = lines[i].find("-->");
    if (arrow == std::string::npos) {
      if (i + 1 >= lines.size() || lines[i + 1].find("-->") == std::string::npos) {
        ++i;  // orphan line: leftover text or junk outside any cue
        continue;
      }
      ts = i + 1;
      arrow = lines[ts].find("-->");
    }

    std::chrono::milliseconds begin{0};
    std::chrono::milliseconds end{0};
    if (!parseTimestamp(lines[ts].substr(0, arrow), begin)) {
      i = ts + 1;
      continue;
    }
    // Past the arrow: the end timestamp, then WebVTT cue settings
    // ("align:start position:0%"), which this parser ignores. Trim first —
    // the arrow is normally followed by a space, and splitting the raw tail
    // on the first blank would hand the timestamp parser an empty field.
    const std::string tail = trim(lines[ts].substr(arrow + 3));
    const std::size_t sp = tail.find_first_of(" \t");
    if (!parseTimestamp(sp == std::string::npos ? tail : tail.substr(0, sp), end)) {
      i = ts + 1;
      continue;
    }

    i = ts + 1;
    std::string text;
    while (i < lines.size() && !isBlank(lines[i]) &&
           lines[i].find("-->") == std::string::npos) {
      if (!text.empty()) {
        text += '\n';
      }
      text += lines[i];
      ++i;
    }
    if (text.empty()) {
      continue;  // a timestamp pair carrying no payload is not a caption
    }

    SubtitleCue cue;
    cue.begin = begin;
    cue.end = end > begin ? end : begin + kDefaultCueDuration;
    cue.text = std::move(text);
    cues.push_back(std::move(cue));
  }
  return cues;
}

bool readSubtitleFile(const std::string& path, std::string& out) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    return false;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  out = std::move(data);
  return true;
}

} // namespace soar
