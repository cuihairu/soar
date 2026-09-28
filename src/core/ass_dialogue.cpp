#include "soar/core/ass_dialogue.h"

#include <fmt/format.h>

#include <string_view>
#include <utility>

namespace soar {

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

} // namespace soar
