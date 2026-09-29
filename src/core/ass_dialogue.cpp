#include "soar/core/ass_dialogue.h"

#include <fmt/format.h>

#include <string_view>
#include <utility>

namespace soar {

namespace {

// "h:mm:ss.cc" -> milliseconds. Tolerant about digit counts (any hour
// width, a 1-3 digit fraction) but strict about the field layout: anything
// else is not an ASS timestamp and fails the caller's cue.
bool parseAssTimestamp(std::string_view s, std::chrono::milliseconds& out) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  const std::size_t c1 = s.find(':');
  const std::size_t c2 = c1 == std::string_view::npos ? std::string_view::npos
                                                      : s.find(':', c1 + 1);
  if (c1 == std::string_view::npos || c2 == std::string_view::npos) {
    return false;
  }
  const std::string_view h = s.substr(0, c1);
  const std::string_view m = s.substr(c1 + 1, c2 - c1 - 1);
  const std::string_view sec = s.substr(c2 + 1);
  const std::size_t dot = sec.find('.');
  if (dot == std::string_view::npos || h.empty() || m.size() != 2) {
    return false;
  }
  const std::string_view ss = sec.substr(0, dot);
  std::string_view frac = sec.substr(dot + 1);
  if (ss.size() != 2 || frac.empty() || frac.size() > 3) {
    return false;
  }
  int values[3] = {0, 0, 0};
  const std::string_view parts[3] = {h, m, ss};
  for (int i = 0; i < 3; ++i) {
    for (const char c : parts[i]) {
      if (c < '0' || c > '9') {
        return false;
      }
      values[i] = values[i] * 10 + (c - '0');
    }
  }
  if (parts[1] > "59" || parts[2] > "59") {
    return false;  // minutes and seconds are two digits, 00-59
  }
  // Fraction -> centiseconds: 1 digit scales up, 3 truncates.
  int cs = (frac[0] - '0') * 10;
  if (frac.size() > 1) cs += frac[1] - '0';
  out = std::chrono::milliseconds(((values[0] * 3600 + values[1] * 60 +
                                    values[2]) * 100 + cs) * 10);
  return true;
}

}  // namespace

std::string assTimestamp(std::chrono::milliseconds ms) {
  if (ms.count() < 0) {
    ms = std::chrono::milliseconds::zero();
  }
  const auto total_cs = static_cast<long long>(ms.count()) / 10;
  const auto h = total_cs / 360000;             // an hour is 360000 cs
  const auto m = (total_cs % 360000) / 6000;    // a minute is 6000 cs
  const auto s = (total_cs % 6000) / 100;
  const auto cs = total_cs % 100;
  return fmt::format("{}:{:02d}:{:02d}.{:02d}", h, m, s, cs);
}

std::string assDialogueLine(const char* ass, std::chrono::milliseconds pts,
                            std::chrono::milliseconds duration) {
  if (ass == nullptr) {
    return {};
  }
  std::string_view s(ass);
  if (!s.empty() && s.front() == '[') {
    s.remove_prefix(1);
  }
  if (!s.empty() && s.back() == ']') {
    s.remove_suffix(1);
  }
  if (s.rfind("Dialogue:", 0) == 0) {
    return std::string(s);
  }
  std::string_view fields[8];
  int nf = 0;
  std::size_t pos = 0;
  while (nf < 8) {
    const std::size_t comma = s.find(',', pos);
    if (comma == std::string_view::npos) {
      break;
    }
    fields[nf++] = s.substr(pos, comma - pos);
    pos = comma + 1;
  }
  if (nf < 8) {
    return std::string(s);
  }
  std::string text(s.substr(pos));
  // A literal newline would end the Dialogue line early; the ASS hard
  // break is the faithful spelling of one.
  if (text.find('\n') != std::string::npos) {
    std::string fixed;
    for (const char c : text) {
      if (c == '\n') {
        fixed += "\\N";
      } else {
        fixed += c;
      }
    }
    text = std::move(fixed);
  }
  return fmt::format("Dialogue: {},{},{},{},{},{},{},{},{},{}",
                     fields[1], assTimestamp(pts), assTimestamp(pts + duration),
                     fields[2], fields[3], fields[4], fields[5], fields[6],
                     fields[7], text);
}

std::vector<SubtitleCue> assDocumentCues(const std::string& content) {
  std::vector<SubtitleCue> cues;
  std::size_t pos = 0;
  while (pos <= content.size()) {
    const std::size_t eol = content.find('\n', pos);
    const std::size_t line_end = eol == std::string::npos ? content.size() : eol;
    std::string_view line(content.data() + pos, line_end - pos);
    pos = eol == std::string::npos ? content.size() + 1 : eol + 1;
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (line.rfind("Dialogue:", 0) != 0) {
      continue;
    }
    line.remove_prefix(9);  // "Dialogue:"
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
      line.remove_prefix(1);
    }
    // layer,start,end,style,name,mL,mR,mV,effect — the text after the
    // ninth comma is field ten and may itself contain commas.
    std::size_t cut = 0;
    int commas = 0;
    std::string_view fields[3];
    while (commas < 9) {
      const std::size_t comma = line.find(',', cut);
      if (comma == std::string_view::npos) {
        break;
      }
      if (commas == 1 || commas == 2) {
        fields[commas] = line.substr(cut, comma - cut);
      }
      cut = comma + 1;
      ++commas;
    }
    if (commas < 9) {
      continue;  // not the 10-field record form
    }
    SubtitleCue cue;
    if (!parseAssTimestamp(fields[1], cue.begin) ||
        !parseAssTimestamp(fields[2], cue.end)) {
      continue;
    }
    // Strip override blocks; an unclosed '{' drops the rest of the text,
    // matching how the embedded rect extraction treats a broken directive.
    std::string text;
    bool in_braces = false;
    for (std::size_t i = cut; i < line.size(); ++i) {
      const char c = line[i];
      if (in_braces) {
        if (c == '}') in_braces = false;
        continue;
      }
      if (c == '{') {
        in_braces = true;
        continue;
      }
      if (c == '\\' && i + 1 < line.size() &&
          (line[i + 1] == 'N' || line[i + 1] == 'n')) {
        text.push_back('\n');
        ++i;
        continue;
      }
      if (c == '\\' && i + 1 < line.size() && line[i + 1] == 'h') {
        text.push_back(' ');
        ++i;
        continue;
      }
      text.push_back(c);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\r')) {
      text.pop_back();
    }
    if (text.empty()) {
      continue;
    }
    cue.text = std::move(text);
    // A cue the file gives no usable end for stays visible for the same
    // default display time the plain-text parser grants.
    if (cue.end <= cue.begin) {
      cue.end = cue.begin + kDefaultCueDuration;
    }
    cues.push_back(std::move(cue));
  }
  return cues;
}

std::string synthesizeAssDocument(const std::vector<SubtitleCue>& cues,
                                  int play_res_x, int play_res_y) {
  // libass's own default play resolution when the caller cannot size the
  // video (the classic v4.00+ script size).
  const int w = play_res_x > 0 ? play_res_x : 384;
  const int h = play_res_y > 0 ? play_res_y : 288;
  // One fourteenth of the frame height, clamped to a readable band: 288p
  // gets the classic 20 px, 1080p gets 77, and the 160x120 test fixture
  // still gets visible glyphs.
  const int font_size = h / 14 < 12 ? 12 : (h / 14 > 96 ? 96 : h / 14);
  const int margin_v = h / 10 < 8 ? 8 : h / 10;
  std::string doc = fmt::format(
      "[Script Info]\n"
      "ScriptType: v4.00+\n"
      "PlayResX: {}\n"
      "PlayResY: {}\n"
      "WrapStyle: 0\n"
      "ScaledBorderAndShadow: yes\n"
      "\n"
      "[V4+ Styles]\n"
      "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
      "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, "
      "ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
      "Alignment, MarginL, MarginR, MarginV, Encoding\n"
      // The family name matches nothing registered (fonts resolve from
      // attachments and AssRenderer's default font), so the default font
      // the UI installs always wins — same fallback a real script gets.
      "Style: Default,Default,{},&H00FFFFFF,&H00FFFFFF,&H00000000,"
      "&H7F000000,0,0,0,0,100,100,0,0,1,2,0,2,10,10,{},1\n"
      "\n"
      "[Events]\n"
      "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, "
      "Effect, Text\n",
      w, h, font_size, margin_v);
  for (const SubtitleCue& cue : cues) {
    // A literal newline would end the Dialogue line early; \N is the ASS
    // hard break, the same respell assDialogueLine applies to payloads.
    std::string text;
    text.reserve(cue.text.size());
    for (const char c : cue.text) {
      if (c == '\n') {
        text += "\\N";
      } else {
        text.push_back(c);
      }
    }
    // Parsed cues already carry the 2 s default for inverted ranges; the
    // max is the same defensive line, for hand-built cue lists.
    const auto end =
        cue.end > cue.begin ? cue.end : cue.begin + kDefaultCueDuration;
    doc += fmt::format("Dialogue: 0,{},{},Default,,0,0,0,,{}\n",
                       assTimestamp(cue.begin), assTimestamp(end), text);
  }
  return doc;
}

} // namespace soar
