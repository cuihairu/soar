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

// TTML offset-time parser: "HH:MM:SS.mmm", "HH:MM:SS,mmm", "HH:MM:SS",
// "MM:SS.mmm" and "SS.mmm". Frame ("100f") and tick ("100t") offsets fail
// here on purpose — converting them needs frameRate/tickRate from the
// <tt> element, and a wrong guess would shift the cue, which the caller
// then skips like any other unparsable timestamp.
bool parseTtmlTimestamp(const std::string& raw, std::chrono::milliseconds& out) {
  const std::string s = trim(raw);
  if (s.empty()) {
    return false;
  }
  if (s.back() == 'f' || s.back() == 't') {
    return false;
  }

  // Try to parse as HH:MM:SS[.mmm] or MM:SS[.mmm] or SS[.mmm]
  // Split by ':' to count components
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (start < s.size()) {
    std::size_t pos = s.find(':', start);
    if (pos == std::string::npos) {
      parts.push_back(s.substr(start));
      break;
    }
    parts.push_back(s.substr(start, pos - start));
    start = pos + 1;
  }

  long long hours = 0, minutes = 0, seconds = 0, millis = 0;

  if (parts.size() == 3) {
    // HH:MM:SS[.mmm]
    if (!parseUnsigned(parts[0], hours) ||
        !parseUnsigned(parts[1], minutes)) {
      return false;
    }
    // Parse seconds with optional fraction
    const std::size_t sep = parts[2].find_first_of(",.");
    if (!parseUnsigned(sep == std::string::npos ? parts[2] : parts[2].substr(0, sep), seconds)) {
      return false;
    }
    if (sep != std::string::npos) {
      std::string frac = parts[2].substr(sep + 1);
      if (frac.empty()) return false;
      for (char c : frac) {
        if (c < '0' || c > '9') return false;
      }
      if (frac.size() > 3) frac.resize(3);
      while (frac.size() < 3) frac += '0';
      millis = (frac[0] - '0') * 100 + (frac[1] - '0') * 10 + (frac[2] - '0');
    }
  } else if (parts.size() == 2) {
    // MM:SS[.mmm]
    if (!parseUnsigned(parts[0], minutes)) return false;
    const std::size_t sep = parts[1].find_first_of(",.");
    if (!parseUnsigned(sep == std::string::npos ? parts[1] : parts[1].substr(0, sep), seconds)) return false;
    if (sep != std::string::npos) {
      std::string frac = parts[1].substr(sep + 1);
      if (frac.empty()) return false;
      for (char c : frac) {
        if (c < '0' || c > '9') return false;
      }
      if (frac.size() > 3) frac.resize(3);
      while (frac.size() < 3) frac += '0';
      millis = (frac[0] - '0') * 100 + (frac[1] - '0') * 10 + (frac[2] - '0');
    }
  } else if (parts.size() == 1) {
    // SS[.mmm]
    const std::size_t sep = parts[0].find_first_of(",.");
    if (!parseUnsigned(sep == std::string::npos ? parts[0] : parts[0].substr(0, sep), seconds)) return false;
    if (sep != std::string::npos) {
      std::string frac = parts[0].substr(sep + 1);
      if (frac.empty()) return false;
      for (char c : frac) {
        if (c < '0' || c > '9') return false;
      }
      if (frac.size() > 3) frac.resize(3);
      while (frac.size() < 3) frac += '0';
      millis = (frac[0] - '0') * 100 + (frac[1] - '0') * 10 + (frac[2] - '0');
    }
  } else {
    return false;
  }

  out = std::chrono::milliseconds(((hours * 60 + minutes) * 60 + seconds) * 1000 + millis);
  return true;
}

// Appends a code point as UTF-8. Out-of-range values return false so the
// caller can keep the raw entity instead of writing a bad sequence.
bool appendUtf8(std::string& out, unsigned long cp) {
  if (cp <= 0x7F) {
    out += static_cast<char>(cp);
  } else if (cp <= 0x7FF) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp <= 0xFFFF) {
    if (cp >= 0xD800 && cp <= 0xDFFF) {
      return false;  // UTF-16 surrogate half: not a code point
    }
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp <= 0x10FFFF) {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    return false;
  }
  return true;
}

// Decodes one character reference ("&amp;", "&#38;", "&#x26;") into `out`.
// Unknown or malformed references are copied verbatim: a forgiving reader
// shows "&#zzz;" rather than swallowing the text around it.
void decodeEntity(const std::string& entity, std::string& out) {
  if (entity == "&lt;") { out += '<'; return; }
  if (entity == "&gt;") { out += '>'; return; }
  if (entity == "&amp;") { out += '&'; return; }
  if (entity == "&quot;") { out += '"'; return; }
  if (entity == "&apos;") { out += '\''; return; }
  if (entity.size() > 3 && entity[1] == '#') {
    const bool hex = entity[2] == 'x' || entity[2] == 'X';
    const std::string digits =
        entity.substr(hex ? 3 : 2, entity.size() - (hex ? 4 : 3));
    unsigned long cp = 0;
    bool ok = !digits.empty();
    const unsigned base = hex ? 16u : 10u;
    for (const char d : digits) {
      unsigned v = 0;
      if (d >= '0' && d <= '9') {
        v = static_cast<unsigned>(d - '0');
      } else if (hex && d >= 'a' && d <= 'f') {
        v = static_cast<unsigned>(d - 'a' + 10);
      } else if (hex && d >= 'A' && d <= 'F') {
        v = static_cast<unsigned>(d - 'A' + 10);
      } else {
        ok = false;
        break;
      }
      cp = cp * base + v;
      if (cp > 0x10FFFF) {
        ok = false;
        break;
      }
    }
    if (ok && appendUtf8(out, cp)) {
      return;
    }
  }
  out += entity;
}

// Extracts the text content of the element whose opening tag ends at
// `pos - 1` style: `pos` goes in just past the '>' and comes out just
// past this element's own closing tag (or end of input). Nested inline
// markup (<span>, <b>, ...) loses its tags but keeps its text, <br/> (any
// namespace prefix) becomes a line break, character references decode to
// UTF-8, and each run of whitespace an indented XML file prints between
// words collapses to a single space — a line break only comes from <br/>.
std::string extractXmlText(const std::string& xml, std::size_t& pos) {
  std::string result;
  int depth = 0;          // inline elements opened inside this one
  bool pending_space = false;

  const auto flushSpace = [&result, &pending_space]() {
    if (pending_space) {
      pending_space = false;
      // Nothing before (leading indent), or a <br/> just landed (the
      // indentation that followed it) — no space to add there.
      if (!result.empty() && result.back() != '\n') {
        result += ' ';
      }
    }
  };

  while (pos < xml.size()) {
    if (xml[pos] == '&') {
      const std::size_t semi = xml.find(';', pos);
      if (semi != std::string::npos && semi - pos <= 12) {
        flushSpace();
        decodeEntity(xml.substr(pos, semi - pos + 1), result);
        pos = semi + 1;
      } else {
        flushSpace();
        result += xml[pos];
        ++pos;
      }
      continue;
    }
    if (xml[pos] != '<') {
      const char c = xml[pos];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        pending_space = true;
      } else {
        flushSpace();
        result += c;
      }
      ++pos;
      continue;
    }

    // Markup. A comment carries no text worth keeping and may contain
    // '>' — it is skipped whole, before the generic tag scan.
    if (xml.compare(pos, 4, "<!--") == 0) {
      const std::size_t end = xml.find("-->", pos + 4);
      pos = end == std::string::npos ? xml.size() : end + 3;
      continue;
    }
    const std::size_t end = xml.find('>', pos);
    if (end == std::string::npos) {
      pos = xml.size();  // truncated file: keep what was read
      break;
    }
    const std::string tag = xml.substr(pos, end - pos + 1);
    pos = end + 1;

    const bool closing = tag.size() >= 2 && tag[1] == '/';
    std::size_t name_start = closing ? 2 : 1;
    std::size_t name_end = tag.find_first_of(" \t\r\n/>", name_start);
    const std::string name =
        tag.substr(name_start, name_end == std::string::npos
                                    ? tag.size() - name_start
                                    : name_end - name_start);
    const std::size_t colon = name.rfind(':');
    const std::string local =
        colon == std::string::npos ? name : name.substr(colon + 1);

    if (closing) {
      if (local == "br") {
        continue;  // </br> after a void <br>: nothing to close
      }
      if (depth == 0) {
        break;  // the caller's element ends here
      }
      --depth;
      continue;
    }
    if (local == "br") {
      // Void element: the only line break this reader produces. Nothing
      // before it (or a br already there) means no empty line.
      if (!result.empty() && result.back() != '\n') {
        pending_space = false;
        result += '\n';
      }
      continue;
    }
    if (name_end != std::string::npos && name_end < tag.size() &&
        tag[name_end] == '/') {
      continue;  // self-closing, not a br: no text, no depth
    }
    ++depth;
  }
  return result;
}

// Reads an attribute value out of an opening tag ("begin='...'" or
// begin='...'), accepting both quote styles. The name must start at a
// word boundary so "end=" does not match inside "send=" or "bl:end=".
// False when the attribute is absent or unquoted.
bool tagAttr(const std::string& tag, const char* name, std::string& out) {
  const std::string key = std::string(name) + "=";
  std::size_t at = tag.find(key);
  while (at != std::string::npos) {
    const char prev = at == 0 ? '<' : tag[at - 1];
    if (prev == ' ' || prev == '\t' || prev == ':' || prev == '<') {
      break;
    }
    at = tag.find(key, at + key.size());
  }
  if (at == std::string::npos) {
    return false;
  }
  const std::size_t v = at + key.size();
  if (v >= tag.size() || (tag[v] != '"' && tag[v] != '\'')) {
    return false;
  }
  const std::size_t close = tag.find(tag[v], v + 1);
  if (close == std::string::npos) {
    return false;
  }
  out = tag.substr(v + 1, close - v - 1);
  return true;
}

// Parse TTML content into cues: every <p> (any namespace prefix) with a
// parsable begin timestamp, its text extracted by extractXmlText. Styling,
// regions and layout are ignored — same plain-cue contract as every other
// text sidecar. A <p> without a usable begin is skipped (its position on
// the timeline is unknowable); a missing or non-advancing end falls back
// to dur when present, then to the default cue duration.
std::vector<SubtitleCue> parseTtml(const std::string& content) {
  std::vector<SubtitleCue> cues;
  if (content.empty()) return cues;

  const std::string& xml = content;
  std::size_t pos = 0;
  while ((pos = xml.find('<', pos)) != std::string::npos) {
    // Tag name up to the first delimiter: "p", "tt:p", "/p", "body", ...
    // Both <p and <tt:p / <ttml:p count as the paragraph element; </p,
    // <pX and a '<' glued mid-word do not (an unescaped '<' in content
    // cannot occur — XML parsers require it escaped).
    std::size_t name_end = xml.find_first_of(" \t\r\n/>", pos + 1);
    if (name_end == std::string::npos) {
      break;  // '<' with no tag terminator: truncated file
    }
    std::string name = xml.substr(pos + 1, name_end - pos - 1);
    const std::size_t colon = name.rfind(':');
    const std::string local =
        colon == std::string::npos ? name : name.substr(colon + 1);
    if (local != "p" || name_end >= xml.size() || xml[name_end] == '/') {
      ++pos;
      continue;
    }

    const std::size_t tag_end = xml.find('>', pos);
    if (tag_end == std::string::npos) {
      break;  // truncated opening tag: no more cues to find
    }
    const std::string tag = xml.substr(pos, tag_end - pos + 1);

    std::string value;
    std::chrono::milliseconds begin{0};
    if (!tagAttr(tag, "begin", value) || !parseTtmlTimestamp(value, begin)) {
      // No usable start time: drop this <p>. Resuming right after its
      // opening tag is safe — content '<' is always escaped, so the next
      // '<' scan hit is the next element, and an inline tag inside this
      // one can only be span/br, never another <p.
      pos = tag_end + 1;
      continue;
    }

    std::chrono::milliseconds end{0};
    bool has_end = tagAttr(tag, "end", value) && parseTtmlTimestamp(value, end);
    if (!has_end && tagAttr(tag, "dur", value)) {
      std::chrono::milliseconds dur{0};
      if (parseTtmlTimestamp(value, dur)) {
        end = begin + dur;
        has_end = true;
      }
    }

    pos = tag_end + 1;
    std::string text = extractXmlText(xml, pos);

    if (text.empty()) {
      continue;  // whitespace-only content collapsed to nothing
    }
    SubtitleCue cue;
    cue.begin = begin;
    cue.end = has_end && end > begin ? end : begin + kDefaultCueDuration;
    cue.text = std::move(text);
    cues.push_back(std::move(cue));
  }
  return cues;
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
  if (ext == ".ttml" || ext == ".dfxp") {
    return SubtitleFormat::Ttml;
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
  // TTML last (the header contract): the first non-blank line after an
  // optional XML declaration must start the <tt> root element, with or
  // without a namespace prefix. An XML-looking file that carries an ASS
  // or "-->" marker above resolves the way it always did; anything else
  // stays Unknown.
  for (const std::string& line : splitLines(content)) {
    if (isBlank(line)) {
      continue;
    }
    const std::string trimmed = trim(line);
    if (trimmed.rfind("<?xml", 0) == 0) {
      continue;  // the declaration is not the root element
    }
    if (trimmed.rfind("<tt", 0) == 0 &&
        (trimmed.size() == 3 || trimmed[3] == ' ' || trimmed[3] == '\t' ||
         trimmed[3] == '>' || trimmed[3] == ':')) {
      return SubtitleFormat::Ttml;
    }
    break;
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

  // TTML uses a completely different XML-based structure
  if (fmt == SubtitleFormat::Ttml) {
    return parseTtml(content);
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
