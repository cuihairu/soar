// Subtitle providers: sidecar scan next to the media and the
// HTTP subtitle download client. The chat translator lives in
// subtitle_provider_translate.cpp; both share the socket
// primitives in subtitle_provider_internal.h.

#include "soar/core/subtitle_provider.h"
#include "subtitle_provider_internal.h"

namespace soar {
namespace {


// The media path behind a MediaSource uri, or "" when the source is not a
// local file (an http stream has no directory to hold sidecars). "file://"
// is stripped; anything without a remote scheme is taken as a path, which
// is how the CLI and the UI both pass local files.
std::string localMediaPath(const std::string& uri) {
  if (uri.rfind("http://", 0) == 0 || uri.rfind("https://", 0) == 0) {
    return {};
  }
  const std::string file_scheme = "file://";
  if (uri.rfind(file_scheme, 0) == 0) {
    return uri.substr(file_scheme.size());
  }
  return uri;
}


// ---------------------------------------------------------------------------
// External (HTTP) subtitle download. The client is deliberately self-contained
// (http only, one blocking connection per request) rather than shared with
// http_cache.cpp: the two stacks serve different lifetimes — a cache streams
// media for minutes, a subtitle request is one small fetch — and keeping them
// apart means neither can regress the other. See the class comment in
// subtitle_provider.h for the wire protocol this speaks.
// ---------------------------------------------------------------------------

constexpr size_t kHashChunkBytes = 64 * 1024;   // head/tail window of the hash


uint64_t hashWords(const uint8_t* p, size_t n, uint64_t acc) {
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t w = 0;
    for (int b = 7; b >= 0; --b) w = (w << 8) | p[i + b];  // little-endian
    acc += w;
  }
  if (i < n) {  // a tail shorter than a word folds into the low bytes
    uint64_t w = 0;
    for (int b = static_cast<int>(n - i) - 1; b >= 0; --b) {
      w = (w << 8) | p[i + b];
    }
    acc += w;
  }
  return acc;
}


// One GET, the whole body. `api_key` rides along as X-API-Key when set and
// never reaches a log line. Anything but a 2xx answer is a failure — this
// client follows no redirects and speaks no chunked encoding.
static bool httpGet(const std::string& url, const std::string& api_key,
                    std::chrono::milliseconds timeout, std::string* body,
                    std::string* err) {
  err->clear();
  body->clear();
  UrlParts u;
  if (!parseHttpUrl(url, &u)) {
    *err = "subtitles: not a usable http:// url: " + url;
    return false;
  }
  const int fd = connectTcp(u, static_cast<unsigned>(timeout.count()), err);
  if (fd < 0) return false;

  std::string req = "GET " + u.path + " HTTP/1.1\r\nHost: " + u.hostport +
                    "\r\nAccept: */*\r\nUser-Agent: soar-subtitles/1\r\n"
                    "Connection: close\r\n";
  if (!api_key.empty()) req += "X-API-Key: " + api_key + "\r\n";
  req += "\r\n";

  std::string head;
  if (!sendAll(fd, req, err) || !recvHeaders(fd, &head, err)) {
    sockClose(fd);
    return false;
  }
  const size_t first_space = head.find(' ');
  if (first_space == std::string::npos) {
    *err = "subtitles: malformed status line";
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
    *err = "subtitles: unexpected status " + std::to_string(status);
    sockClose(fd);
    return false;
  }
  if (has_length && content_length > kMaxDownloadBytes) {
    *err = "subtitles: response of " + std::to_string(content_length) +
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
          fd, buf, static_cast<int>(std::min<size_t>(content_length - got, sizeof(buf))), 0));
      if (r <= 0) {
        *err = "subtitles: connection closed mid-body (" + std::to_string(got) + "/" +
               std::to_string(content_length) + " bytes)";
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


std::string percentEncode(const std::string& s) {
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                            c == '.' || c == '~';
    if (unreserved) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0xf]);
    }
  }
  return out;
}


// One candidate per line, four tab-separated fields (see the header). Bad
// lines are skipped, never fatal — a half-broken catalog still yields its
// parseable half.
std::vector<SubtitleCandidate> parseCandidateLines(const std::string& body) {
  std::vector<SubtitleCandidate> out;
  std::istringstream is(body);
  std::string line;
  while (std::getline(is, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> fields;
    size_t pos = 0;
    while (fields.size() < 5) {
      const size_t tab = line.find('\t', pos);
      if (tab == std::string::npos) {
        fields.push_back(line.substr(pos));
        break;
      }
      fields.push_back(line.substr(pos, tab - pos));
      pos = tab + 1;
    }
    if (fields.size() < 4 || fields[0].empty()) continue;
    if (fields[0].rfind("http://", 0) != 0) continue;  // no TLS client here
    const std::string ext = toLower(fields[3]);
    SubtitleFormat format = SubtitleFormat::Unknown;
    if (ext == "srt" || ext == "subrip") {
      format = SubtitleFormat::SubRip;
    } else if (ext == "vtt" || ext == "webvtt") {
      format = SubtitleFormat::WebVtt;
    } else if (ext == "ass" || ext == "ssa") {
      format = SubtitleFormat::Ass;
    } else if (ext == "ttml" || ext == "dfxp") {
      format = SubtitleFormat::Ttml;
    } else {
      continue;  // only formats the core parser reads are offered
    }
    SubtitleCandidate cand;
    cand.path = fields[0];
    cand.language = fields[1];
    cand.title = fields[2].empty() ? fields[0] : fields[2];
    cand.format = format;
    out.push_back(std::move(cand));
  }
  return out;
}


// Builds "{endpoint}?size=..&hash=..&name=..", appending with & when the
// endpoint already carries a query of its own.
std::string searchUrl(const HttpSubtitleConfig& cfg, const std::string& size,
                      const std::string& hash, const std::string& name) {
  std::string url = cfg.endpoint;
  url += url.find('?') == std::string::npos ? '?' : '&';
  url += "size=" + size + "&hash=" + hash + "&name=" + name;
  return url;
}

} // namespace


std::vector<SubtitleCandidate> SidecarSubtitleProvider::findCandidates(
    const MediaSource& source) const {
  std::vector<SubtitleCandidate> out;

  const std::string local = localMediaPath(source.uri);
  if (local.empty()) {
    return out;
  }
  std::error_code ec;
  const std::filesystem::path media(local);
  // Not a media file (missing, a directory, unreadable): nothing to look for
  // sidecars next to. is_regular_file answers false instead of throwing.
  if (!std::filesystem::is_regular_file(media, ec)) {
    return out;
  }

  const std::string stem = toLower(media.stem().string());
  const std::size_t sep = stem.size();  // a candidate must begin "<stem>."
  const std::filesystem::path dir =
      media.parent_path().empty() ? std::filesystem::path(".") : media.parent_path();
  // The directory is the media's own, so it exists; a permission-denied
  // entry is skipped rather than fatal (skip_permission_denied below).

  std::filesystem::directory_iterator it(
      dir, std::filesystem::directory_options::skip_permission_denied, ec);
  for (const std::filesystem::directory_entry& entry : it) {
    if (!entry.is_regular_file(ec)) {
      continue;  // a directory or a device that merely looks like a sidecar
    }
    const std::string name = entry.path().filename().string();
    const std::string lower = toLower(name);
    // "<stem>" + "." + a non-empty extension, matched case-insensitively.
    if (lower.size() < sep + 2 || lower.compare(0, sep, stem) != 0 ||
        lower[sep] != '.') {
      continue;
    }
    // name[sep] is a dot, so the last one is at or after it: the text
    // between them is the tag ("en", "en.forced", "" when untagged).
    const std::size_t dot = lower.rfind('.');
    const SubtitleFormat format = subtitleFormatFromPath(name);
    if (format == SubtitleFormat::Unknown) {
      continue;  // bitmap subs (PGS, VobSub) need a decoder, not a parser
    }

    SubtitleCandidate cand;
    cand.path = entry.path().string();
    cand.format = format;
    cand.title = name;
    // dot == sep for an untagged file ("movie.srt"), so the tag is empty
    // there rather than the tail of the extension.
    const std::string tag = lower.substr(sep + 1, dot > sep ? dot - sep - 1 : 0);
    // "movie.en.forced.srt" -> language "en"; the rest of the tag ("forced")
    // stays out of the language field.
    cand.language = tag.substr(0, tag.find('.'));
    out.push_back(std::move(cand));
  }

  // Untagged first, then alphabetical: the common single-subtitle file leads
  // the menu and the order never depends on directory iteration order.
  std::sort(out.begin(), out.end(), [](const SubtitleCandidate& a, const SubtitleCandidate& b) {
    const bool a_plain = a.language.empty();
    const bool b_plain = b.language.empty();
    if (a_plain != b_plain) {
      return a_plain;
    }
    return a.title < b.title;
  });
  return out;
}


bool SidecarSubtitleProvider::fetch(const SubtitleCandidate& candidate, std::string& out) const {
  return readSubtitleFile(candidate.path, out);
}


// ---------------------------------------------------------------------------
// ExternalSubtitleProvider
// ---------------------------------------------------------------------------

ExternalSubtitleProvider::ExternalSubtitleProvider(HttpSubtitleConfig config)
    : config_(std::move(config)) {}


void ExternalSubtitleProvider::configure(HttpSubtitleConfig config) {
  config_ = std::move(config);
}


std::vector<SubtitleCandidate> ExternalSubtitleProvider::findCandidates(
    const MediaSource& source) const {
  // Unconfigured means offline: not even a name lookup, per the "no forced
  // networking" rule. The UI shows this as simply "no external subtitles".
  if (config_.endpoint.empty()) {
    return {};
  }
  const std::string local = localMediaPath(source.uri);
  if (local.empty()) {
    return {};  // an http stream has no bytes here to hash
  }
  const std::string hash = mediaHashHex(local);
  if (hash.empty()) {
    return {};  // not a readable file: nothing to identify it by
  }

  std::error_code ec;
  const uintmax_t size = std::filesystem::file_size(local, ec);
  const std::string size_str = ec ? "0" : std::to_string(size);
  const std::string name =
      percentEncode(std::filesystem::path(local).stem().string());

  std::string body;
  std::string err;
  if (!httpGet(searchUrl(config_, size_str, hash, name), config_.api_key,
               config_.timeout, &body, &err)) {
    return {};  // unreachable, timeout, non-2xx: degrade to no candidates
  }
  return parseCandidateLines(body);
}


bool ExternalSubtitleProvider::fetch(const SubtitleCandidate& candidate,
                                     std::string& out) const {
  std::string body;
  std::string err;
  if (!httpGet(candidate.path, config_.api_key, config_.timeout, &body, &err)) {
    return false;
  }
  // Only bytes the core parser genuinely understands count as a subtitle;
  // an HTML error page served with a 200 must not become a track.
  if (detectSubtitleFormat(body) == SubtitleFormat::Unknown) {
    return false;
  }
  out = std::move(body);
  return true;
}


// ---------------------------------------------------------------------------
// mediaHashHex / storeExternalSubtitle
// ---------------------------------------------------------------------------

std::string mediaHashHex(const std::string& media_path) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(media_path, ec)) {
    return {};
  }
  FILE* f = std::fopen(media_path.c_str(), "rb");
  if (!f) {
    return {};
  }
  const uintmax_t size = std::filesystem::file_size(media_path, ec);
  if (ec) {
    std::fclose(f);
    return {};
  }

  // sum of the unsigned 64-bit little-endian words of the head and tail
  // 64 KiB plus the size — the recipe several subtitle services use, so a
  // server can compute the same digest without this client.
  std::vector<uint8_t> buf(kHashChunkBytes);
  uint64_t acc = 0;

  const size_t head = static_cast<size_t>(
      std::min<uintmax_t>(size, kHashChunkBytes));
  if (std::fread(buf.data(), 1, head, f) != head) {
    std::fclose(f);
    return {};
  }
  acc = hashWords(buf.data(), head, acc);

  if (size > kHashChunkBytes) {
    const size_t tail = kHashChunkBytes;
#ifdef _WIN32
    const int seeked = _fseeki64(f, static_cast<__int64>(size - tail), SEEK_SET);
#else
    const int seeked = std::fseek(f, static_cast<long>(size - tail), SEEK_SET);
#endif
    if (seeked != 0 || std::fread(buf.data(), 1, tail, f) != tail) {
      std::fclose(f);
      return {};
    }
    acc = hashWords(buf.data(), tail, acc);
  }
  std::fclose(f);

  acc += size;
  char hex[17];
  std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(acc));
  return hex;
}


std::string storeExternalSubtitle(const std::string& dir,
                                  const SubtitleCandidate& candidate,
                                  const std::string& text) {
  std::error_code ec;
  std::filesystem::path base =
      dir.empty() ? std::filesystem::temp_directory_path(ec)
                  : std::filesystem::path(dir);
  if (dir.empty() && ec) {
    return {};  // not even a temp directory: give up silently
  }

  const std::filesystem::path target = base / "subtitles";
  std::filesystem::create_directories(target, ec);
  if (ec) {
    return {};
  }

  // A title from a remote catalog is untrusted input: anything outside the
  // safe set becomes '_', so it cannot climb out of the subtitles directory.
  std::string name;
  name.reserve(candidate.title.size());
  for (char c : candidate.title) {
    const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    name.push_back(safe ? c : '_');
  }
  if (name.empty() || name == "." || name == "..") {
    name = "subtitle";
  }
  const size_t dot = name.rfind('.');
  if (dot == std::string::npos || dot == 0) {
    name += candidate.format == SubtitleFormat::WebVtt  ? ".vtt"
            : candidate.format == SubtitleFormat::Ass   ? ".ass"
            : candidate.format == SubtitleFormat::Ttml  ? ".ttml"
                                                       : ".srt";
  }

  const std::filesystem::path file = target / name;
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  if (!out) {
    return {};
  }
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  out.flush();
  if (!out) {
    return {};
  }
  return file.string();
}

} // namespace soar
