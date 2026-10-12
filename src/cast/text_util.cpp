#include "text_util.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace soar::cast {

std::string lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

bool ciEqual(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  }
  return true;
}

namespace {

bool isBoundary(char c) { return c == ' ' || c == '\t'; }

// True iff `text` at `pos` starts "<" + tag + a character that ends an
// opening tag. The boundary is what keeps "serviceType" from matching
// "serviceTypeId".
bool tagBeginsAt(std::string_view text, size_t pos, const char* tag) {
  const size_t n = std::strlen(tag);
  if (pos + 1 + n >= text.size()) return false;
  if (text[pos] != '<' || !ciEqual(text.substr(pos + 1, n), tag)) return false;
  return isBoundary(text[pos + 1 + n]) || text[pos + 1 + n] == '>';
}

// True iff `text` at `pos` starts "</" + tag + (">" or whitespace).
bool tagEndsAt(std::string_view text, size_t pos, const char* tag) {
  const size_t n = std::strlen(tag);
  if (pos + 2 + n >= text.size()) return false;
  if (text[pos] != '<' || text[pos + 1] != '/' || !ciEqual(text.substr(pos + 2, n), tag))
    return false;
  return text[pos + 2 + n] == '>' || isBoundary(text[pos + 2 + n]);
}

size_t findOpeningTag(std::string_view text, size_t from, const char* tag) {
  for (size_t i = from; i < text.size(); ++i) {
    if (text[i] == '<' && tagBeginsAt(text, i, tag)) return i;
  }
  return std::string::npos;
}

size_t findClosingTag(std::string_view text, size_t from, const char* tag) {
  for (size_t i = from; i < text.size(); ++i) {
    if (text[i] == '<' && tagEndsAt(text, i, tag)) return i;
  }
  return std::string::npos;
}

std::string trimBlanks(std::string_view text) {
  size_t b = 0, e = text.size();
  const auto blank = [](char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
  };
  while (b < e && blank(text[b])) ++b;
  while (e > b && blank(text[e - 1])) --e;
  return std::string(text.substr(b, e - b));
}

}  // namespace

std::string fieldValue(std::string_view raw, const char* name) {
  const std::string want = lower(name);
  size_t from = 0;
  while (from <= raw.size()) {
    const size_t nl = raw.find('\n', from);
    std::string_view line(raw.data() + from,
                          nl == std::string::npos ? raw.size() - from : nl - from);
    size_t eol = line.size();
    while (eol > 0 && (line[eol - 1] == '\r' || isBoundary(line[eol - 1]))) --eol;
    const size_t colon = line.substr(0, eol).find(':');
    if (colon != std::string::npos) {
      size_t nlen = colon;
      while (nlen > 0 && isBoundary(line[nlen - 1])) --nlen;
      if (nlen == want.size() && ciEqual(line.substr(0, nlen), want)) {
        size_t v = colon + 1;
        while (v < eol && isBoundary(line[v])) ++v;
        return std::string(line.substr(v, eol - v));
      }
    }
    if (nl == std::string::npos) break;
    from = nl + 1;
  }
  return "";
}

std::string tagText(std::string_view xml, const char* tag) {
  const size_t open = findOpeningTag(xml, 0, tag);
  if (open == std::string::npos) return "";
  const size_t gt = xml.find('>', open);
  if (gt == std::string::npos) return "";
  if (gt > open + 1 && xml[gt - 1] == '/') return "";  // self-closing
  const size_t close = findClosingTag(xml, gt + 1, tag);
  if (close == std::string::npos) return "";
  return trimBlanks(xmlDecode(xml.substr(gt + 1, close - gt - 1)));
}

std::vector<std::string> tagTexts(std::string_view xml, const char* tag) {
  std::vector<std::string> out;
  size_t from = 0;
  while (from < xml.size()) {
    const size_t open = findOpeningTag(xml, from, tag);
    if (open == std::string::npos) break;
    const size_t gt = xml.find('>', open);
    if (gt == std::string::npos) break;
    // "<tag/>" carries no value but is still a repeatable entry: the
    // caller counts occurrences to know a service is present.
    if (gt > open + 1 && xml[gt - 1] == '/') {
      out.emplace_back();
      from = gt + 1;
      continue;
    }
    const size_t close = findClosingTag(xml, gt + 1, tag);
    if (close == std::string::npos) break;
    out.push_back(trimBlanks(xmlDecode(xml.substr(gt + 1, close - gt - 1))));
    from = close + 1;
  }
  return out;
}

// Value of `attr` inside the opening tag's head: the text between the tag
// name and the closing bracket, scanned as name = quote value quote. The
// tag name itself carries no '=', so it never parses as an attribute.
std::string tagAttr(std::string_view xml, const char* tag, const char* attr) {
  const size_t open = findOpeningTag(xml, 0, tag);
  if (open == std::string::npos) return "";
  const size_t gt = xml.find('>', open);
  if (gt == std::string::npos) return "";
  const std::string_view head = xml.substr(open + 1, gt - open - 1);
  const std::string want = lower(attr);
  size_t i = 0;
  while (i < head.size() && !isBoundary(head[i])) ++i;  // past the tag name
  while (i < head.size()) {
    while (i < head.size() && isBoundary(head[i])) ++i;
    const size_t name_at = i;
    while (i < head.size() && head[i] != '=' && !isBoundary(head[i])) ++i;
    if (i == name_at) break;  // no more names
    const std::string name = lower(head.substr(name_at, i - name_at));
    while (i < head.size() && isBoundary(head[i])) ++i;
    if (i >= head.size() || head[i] != '=') continue;  // a bare word, not attr
    ++i;
    while (i < head.size() && isBoundary(head[i])) ++i;
    if (i >= head.size()) break;
    const char quote = head[i++];
    if (quote != '"' && quote != '\'') break;  // malformed: give up
    const size_t value_at = i;
    while (i < head.size() && head[i] != quote) ++i;
    if (i >= head.size()) break;  // unterminated
    if (name == want) return xmlDecode(head.substr(value_at, i - value_at));
    ++i;  // past the closing quote
  }
  return "";
}

std::string xmlDecode(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  size_t i = 0;
  while (i < text.size()) {
    if (text[i] != '&') {
      out.push_back(text[i++]);
      continue;
    }
    const size_t semi = text.find(';', i + 1);
    if (semi == std::string::npos || semi > i + 12) {
      out.push_back(text[i++]);
      continue;
    }
    const std::string_view entity(text.data() + i + 1, semi - i - 1);
    if (entity == "amp") {
      out.push_back('&');
    } else if (entity == "lt") {
      out.push_back('<');
    } else if (entity == "gt") {
      out.push_back('>');
    } else if (entity == "quot") {
      out.push_back('"');
    } else if (entity == "apos") {
      out.push_back('\'');
    } else if (!entity.empty() && entity[0] == '#') {
      const bool hex = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X');
      const std::string digits(entity.substr(hex ? 2 : 1));
      if (digits.empty() ||
          digits.find_first_not_of(hex ? "0123456789abcdefABCDEF" : "0123456789") !=
              std::string::npos) {
        out.push_back(text[i++]);  // "#", "#x" or non-numeric: not an entity
      } else {
        const unsigned long code = std::strtoul(digits.c_str(), nullptr, hex ? 16 : 10);
        if (code < 128) {
          out.push_back(static_cast<char>(code));
        } else {
          // A multi-byte point is outside anything the parsers compare;
          // keep it readable rather than inventing an encoding here.
          out.push_back(static_cast<char>(code & 0xff));
        }
      }
    } else {
      out.append(text.data() + i, semi - i + 1);  // unknown escape: keep it
    }
    i = semi + 1;
  }
  return out;
}

std::string xmlEscape(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (const char c : text) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default: out.push_back(c);
    }
  }
  return out;
}

int httpStatus(std::string_view response) {
  const size_t nl = response.find('\n');
  std::string_view line(response.data(), nl == std::string::npos ? response.size() : nl);
  size_t eol = line.size();
  while (eol > 0 && (line[eol - 1] == '\r' || isBoundary(line[eol - 1]))) --eol;
  // "HTTP/1.1 200 OK" (the version digits belong to the token).
  if (eol < 9 || !ciEqual(line.substr(0, 5), "HTTP/")) return 0;
  const size_t sp = line.find(' ');
  if (sp == std::string::npos || sp + 4 > eol) return 0;
  const std::string code(line.substr(sp + 1, 3));
  if (code.find_first_not_of("0123456789") != std::string::npos) return 0;
  return std::atoi(code.c_str());
}

}  // namespace soar::cast
