// SubtitleTranslator: the chat-completion translation client.
// Split out of subtitle_provider.cpp; shares the socket
// primitives in subtitle_provider_internal.h.

#include "soar/core/subtitle_provider.h"
#include "subtitle_provider_internal.h"

// ---------------------------------------------------------------------------
// SubtitleTranslator: batch text translation over an OpenAI-compatible chat
// endpoint. Same self-contained blocking client as the download provider
// above — one small request at a time — plus the smallest JSON writing and
// reading the documented request/response shapes need (no JSON library,
// like the rest of the core; the shapes are pinned by the header comment
// and by the fixture-server tests).
// ---------------------------------------------------------------------------

namespace soar {
namespace {


// Whitespace as the payload builder sees it: explicit, locale-independent,
// and safe on UTF-8 bytes (a plain isspace(char) would be UB on bytes
// >= 0x80, which every non-ASCII subtitle is full of).
bool isPayloadSpace(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' ||
         ch == '\f' || ch == '\v';
}


// Collapses every whitespace run (cue line breaks included) to one space
// and trims the ends: the wire carries a cue as a single numbered line,
// because a timed caption is one thought. parseSubtitleText never emits an
// empty cue, so this never returns "" for a parsed cue.
std::string collapseLines(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  bool pending_space = false;
  for (char ch : s) {
    if (isPayloadSpace(ch)) {
      pending_space = !out.empty();
    } else {
      if (pending_space) {
        out.push_back(' ');
        pending_space = false;
      }
      out.push_back(ch);
    }
  }
  return out;
}


// Escapes a UTF-8 string as the body of a JSON string (no surrounding
// quotes). Bytes above 0x7f pass through untouched: the payload is valid
// UTF-8 in, valid UTF-8 out.
std::string jsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(ch);
        }
    }
  }
  return out;
}


// Decodes the JSON string literal starting at s[pos] == '"' into `out`.
// False when the literal never terminates or carries an unknown escape.
// \uXXXX escapes are decoded (surrogate pairs included) — endpoints that
// answer ASCII-safe JSON must not leave \uXXXX literal inside translated
// text. A surrogate without a partner becomes U+FFFD, never raw UTF-8 of a
// surrogate value (that would not be valid UTF-8).
bool jsonUnescapeString(const std::string& s, size_t pos, std::string* out) {
  if (pos >= s.size() || s[pos] != '"') {
    return false;
  }
  ++pos;
  out->clear();
  auto hex4 = [&](unsigned* v) {
    if (pos + 4 > s.size()) {
      return false;
    }
    *v = 0;
    for (int i = 0; i < 4; ++i) {
      const char h = s[pos + i];
      *v <<= 4;
      if (h >= '0' && h <= '9') {
        *v |= static_cast<unsigned>(h - '0');
      } else if (h >= 'a' && h <= 'f') {
        *v |= static_cast<unsigned>(h - 'a' + 10);
      } else if (h >= 'A' && h <= 'F') {
        *v |= static_cast<unsigned>(h - 'A' + 10);
      } else {
        return false;
      }
    }
    pos += 4;
    return true;
  };
  auto appendUtf8 = [&](unsigned cp) {
    if (cp < 0x80) {
      out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  };
  while (pos < s.size()) {
    const char ch = s[pos++];
    if (ch == '"') {
      return true;
    }
    if (ch != '\\') {
      out->push_back(ch);
      continue;
    }
    if (pos >= s.size()) {
      return false;
    }
    const char e = s[pos++];
    switch (e) {
      case '"': out->push_back('"'); break;
      case '\\': out->push_back('\\'); break;
      case '/': out->push_back('/'); break;
      case 'b': out->push_back('\b'); break;
      case 'f': out->push_back('\f'); break;
      case 'n': out->push_back('\n'); break;
      case 'r': out->push_back('\r'); break;
      case 't': out->push_back('\t'); break;
      case 'u': {
        unsigned cp = 0;
        if (!hex4(&cp)) {
          return false;
        }
        if (cp >= 0xD800 && cp <= 0xDBFF && pos + 1 < s.size() &&
            s[pos] == '\\' && s[pos + 1] == 'u') {
          const size_t save = pos;
          pos += 2;
          unsigned lo = 0;
          if (hex4(&lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          } else {
            pos = save;
            cp = 0xFFFD;
          }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
          cp = 0xFFFD;
        }
        appendUtf8(cp);
        break;
      }
      default:
        return false;
    }
  }
  return false;  // unterminated literal
}


// Reads choices[0].message.content from the documented response shape:
// locates "choices", the array, its first object, then "message", then
// "content". Anything missing is not the documented shape and fails the
// translation — the extractor is deliberately not a general JSON parser.
bool chatResponseContent(const std::string& body, std::string* out) {
  size_t p = body.find("\"choices\"");
  if (p == std::string::npos) return false;
  p = body.find('[', p);
  if (p == std::string::npos) return false;
  p = body.find('{', p);
  if (p == std::string::npos) return false;
  p = body.find("\"message\"", p);
  if (p == std::string::npos) return false;
  p = body.find("\"content\"", p);
  if (p == std::string::npos) return false;
  p = body.find(':', p);
  if (p == std::string::npos) return false;
  ++p;
  while (p < body.size() && isPayloadSpace(body[p])) ++p;
  return jsonUnescapeString(body, p, out);
}


// "{endpoint}/chat/completions", tolerating a trailing slash on the
// endpoint and an endpoint that already spells the full path.
std::string chatUrl(const std::string& endpoint) {
  static const char kTail[] = "/chat/completions";
  std::string base = endpoint;
  while (!base.empty() && base.back() == '/') {
    base.pop_back();
  }
  const size_t tail_len = sizeof(kTail) - 1;
  if (base.size() >= tail_len &&
      base.compare(base.size() - tail_len, tail_len, kTail) == 0) {
    return base;
  }
  return base + kTail;
}


// "1. first cue\n2. second cue" — numbering restarts at 1 per batch, and
// the reply is mapped back by position, not by number.
std::string numberedPayload(const std::vector<std::string>& texts) {
  std::string out;
  for (size_t i = 0; i < texts.size(); ++i) {
    if (i > 0) out += '\n';
    out += std::to_string(i + 1);
    out += ". ";
    out += texts[i];
  }
  return out;
}


// Maps the numbered reply lines back to exactly `count` translations, in
// order. False unless every non-blank reply line is numbered ("12.",
// "12)" or "12:") and the count matches — a reply that answers only part
// of the batch must fail the whole translation, never truncate the track.
bool parseNumberedReply(const std::string& content, size_t count,
                        std::vector<std::string>* out) {
  out->clear();
  size_t pos = 0;
  for (;;) {
    const size_t eol = content.find('\n', pos);
    std::string line =
        content.substr(pos, eol == std::string::npos ? eol : eol - pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty()) {
      size_t start = 0;
      while (start < line.size() && line[start] == ' ') ++start;
      size_t digits = start;
      while (digits < line.size() && line[digits] >= '0' &&
             line[digits] <= '9') {
        ++digits;
      }
      if (digits == start) return false;  // not numbered at all
      if (digits >= line.size() ||
          (line[digits] != '.' && line[digits] != ')' &&
           line[digits] != ':')) {
        return false;
      }
      size_t after = digits + 1;
      if (after < line.size() && (line[after] == ' ' || line[after] == '\t')) {
        ++after;
      }
      out->push_back(line.substr(after));
    }
    if (eol == std::string::npos) break;
    pos = eol + 1;
  }
  return out->size() == count;
}


std::string formatTimestamp(std::chrono::milliseconds t, char ms_sep) {
  const long long total = t.count();
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld%c%03lld",
                total / 3600000, (total / 60000) % 60, (total / 1000) % 60,
                ms_sep, total % 1000);
  return buf;
}


// Renders cues back into the input's container format. Cue numbering in
// SubRip follows the file position, so a translated file lines up with
// its source one. Timestamps come from the parser and are non-negative.
std::string serializeSubtitle(const std::vector<SubtitleCue>& cues,
                              SubtitleFormat format) {
  std::string out;
  if (format == SubtitleFormat::WebVtt) {
    out += "WEBVTT\n\n";
  }
  const char ms_sep = format == SubtitleFormat::WebVtt ? '.' : ',';
  for (size_t i = 0; i < cues.size(); ++i) {
    if (format == SubtitleFormat::SubRip) {
      out += std::to_string(i + 1);
      out += '\n';
    }
    out += formatTimestamp(cues[i].begin, ms_sep);
    out += " --> ";
    out += formatTimestamp(cues[i].end, ms_sep);
    out += '\n';
    out += cues[i].text;
    out += "\n\n";
  }
  return out;
}


// One blocking POST with a JSON body; the transport twin of httpGet above
// (no redirects, no chunked encoding, anything but 2xx is a failure). The
// response cap is the same download cap: a translation reply is small, and
// a runaway endpoint must not stream into memory forever.
bool httpPost(const std::string& url, const std::string& api_key,
              const std::string& json_body, std::chrono::milliseconds timeout,
              std::string* body, std::string* err) {
  err->clear();
  body->clear();
  UrlParts u;
  if (!parseHttpUrl(url, &u)) {
    *err = "translate: not a usable http:// url: " + url;
    return false;
  }
  const int fd = connectTcp(u, static_cast<unsigned>(timeout.count()), err);
  if (fd < 0) return false;

  std::string req = "POST " + u.path + " HTTP/1.1\r\nHost: " + u.hostport +
                    "\r\nAccept: application/json\r\n"
                    "User-Agent: soar-subtitles/1\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: " +
                    std::to_string(json_body.size()) +
                    "\r\n"
                    "Connection: close\r\n";
  if (!api_key.empty()) req += "Authorization: Bearer " + api_key + "\r\n";
  req += "\r\n";
  req += json_body;

  std::string head;
  if (!sendAll(fd, req, err) || !recvHeaders(fd, &head, err)) {
    sockClose(fd);
    return false;
  }
  const size_t first_space = head.find(' ');
  if (first_space == std::string::npos) {
    *err = "translate: malformed status line";
    sockClose(fd);
    return false;
  }
  const int status = std::atoi(head.c_str() + first_space + 1);

  size_t content_length = 0;
  bool has_length = false;
  {
    size_t line = head.find("\r\n");
    line = line == std::string::npos ? std::string::npos : line + 2;
    while (line != std::string::npos && line < head.size()) {
      const size_t eol = head.find("\r\n", line);
      const std::string h = toLower(head.substr(
          line, (eol == std::string::npos ? head.size() : eol) - line));
      if (h.rfind("content-length:", 0) == 0) {
        const long v = std::atol(h.c_str() + 15);
        if (v >= 0) {
          content_length = static_cast<size_t>(v);
          has_length = true;
        }
      }
      line = eol == std::string::npos ? std::string::npos : eol + 2;
    }
  }

  if (status < 200 || status >= 300) {
    *err = "translate: unexpected status " + std::to_string(status);
    sockClose(fd);
    return false;
  }
  if (has_length && content_length > kMaxDownloadBytes) {
    *err = "translate: response of " + std::to_string(content_length) +
           " bytes exceeds the download cap";
    sockClose(fd);
    return false;
  }

  bool ok;
  if (has_length) {
    body->reserve(content_length);
    char buf[16 * 1024];
    size_t got = 0;
    while (got < content_length) {
      const int r = static_cast<int>(recv(
          fd, buf,
          static_cast<int>(std::min<size_t>(content_length - got, sizeof(buf))),
          0));
      if (r <= 0) {
        *err = "translate: connection closed mid-body (" + std::to_string(got) +
               "/" + std::to_string(content_length) + " bytes)";
        break;
      }
      body->append(buf, static_cast<size_t>(r));
      got += static_cast<size_t>(r);
    }
    ok = got == content_length;
  } else {
    ok = recvSome(fd, body, kMaxDownloadBytes, err);
  }
  sockClose(fd);
  return ok;
}

} // namespace


SubtitleTranslator::SubtitleTranslator(SubtitleTranslateConfig config)
    : config_(std::move(config)) {}


void SubtitleTranslator::configure(SubtitleTranslateConfig config) {
  config_ = std::move(config);
}


bool SubtitleTranslator::translate(const std::string& subtitle_text,
                                   const std::string& target_language,
                                   std::string* out_text,
                                   std::string* err) const {
  if (err != nullptr) err->clear();
  if (out_text != nullptr) out_text->clear();
  // The transport layer writes its reason unconditionally; callers that
  // don't want it still get a safe sink.
  std::string scratch_err;
  std::string* err_sink = err != nullptr ? err : &scratch_err;

  // Unconfigured means offline: no dial, a clear refusal — per the "no
  // forced networking" rule. The model name is part of the configuration:
  // an OpenAI-compatible endpoint needs one, and without it the request
  // shape is not defined.
  if (config_.endpoint.empty() || config_.model.empty()) {
    if (err != nullptr) {
      *err = "translate: not configured (endpoint and model are required)";
    }
    return false;
  }
  if (config_.endpoint.rfind("http://", 0) != 0) {
    if (err != nullptr) {
      *err = "translate: only http:// endpoints are supported: " +
             config_.endpoint;
    }
    return false;
  }
  if (target_language.empty()) {
    if (err != nullptr) {
      *err = "translate: no target language given";
    }
    return false;
  }
  const SubtitleFormat format = detectSubtitleFormat(subtitle_text);
  // Only SubRip/WebVTT are translator input: the wire format is numbered
  // plain-text lines and serializeSubtitle renders back SubRip/WebVTT. An
  // ASS/SSA document or a TTML file would parse into cues but serialize
  // into a container that is not its own — a row that can only mangle.
  if (format != SubtitleFormat::SubRip && format != SubtitleFormat::WebVtt) {
    if (err != nullptr) {
      *err = "translate: input is not SubRip or WebVTT text";
    }
    return false;
  }
  const std::vector<SubtitleCue> cues = parseSubtitleText(subtitle_text, format);
  if (cues.empty()) {
    if (err != nullptr) {
      *err = "translate: input carries no cues";
    }
    return false;
  }

  // The wire text per cue: one line, trimmed. parseSubtitleText never
  // emits an empty cue, so `texts` has exactly one entry per cue and the
  // reply maps back by position.
  std::vector<std::string> texts;
  texts.reserve(cues.size());
  for (const SubtitleCue& cue : cues) {
    texts.push_back(collapseLines(cue.text));
  }

  std::vector<std::string> translated(cues.size());
  const std::string system =
      "You translate subtitle cues. Translate each numbered line into " +
      target_language +
      ". Reply with exactly the same number of numbered lines, in the same "
      "order, translations only - no notes, no originals.";
  const size_t batch = config_.batch_cues < 1 ? 1 : config_.batch_cues;
  for (size_t first = 0; first < texts.size(); first += batch) {
    const size_t last = std::min<size_t>(texts.size(), first + batch);
    const std::vector<std::string> chunk(texts.begin() + static_cast<long>(first),
                                         texts.begin() + static_cast<long>(last));
    std::string payload =
        "{\"model\":\"" + jsonEscape(config_.model) +
        "\",\"temperature\":0,\"messages\":[{\"role\":\"system\",\"content\":\"" +
        jsonEscape(system) + "\"},{\"role\":\"user\",\"content\":\"" +
        jsonEscape(numberedPayload(chunk)) + "\"}]}";

    std::string reply;
    if (!httpPost(chatUrl(config_.endpoint), config_.api_key, payload,
                  config_.timeout, &reply, err_sink)) {
      return false;  // err already carries the transport reason
    }
    std::string content;
    if (!chatResponseContent(reply, &content)) {
      if (err != nullptr) {
        *err = "translate: response carries no choices[0].message.content";
      }
      return false;
    }
    std::vector<std::string> lines;
    if (!parseNumberedReply(content, chunk.size(), &lines)) {
      if (err != nullptr) {
        *err = "translate: reply does not carry one numbered line per cue";
      }
      return false;
    }
    for (size_t j = 0; j < lines.size(); ++j) {
      translated[first + j] = std::move(lines[j]);
    }
  }

  if (out_text != nullptr) {
    std::vector<SubtitleCue> out = cues;
    for (size_t i = 0; i < out.size(); ++i) {
      out[i].text = std::move(translated[i]);
    }
    *out_text = serializeSubtitle(out, format);
  }
  return true;
}

} // namespace soar
