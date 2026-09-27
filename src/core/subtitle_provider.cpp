#include "soar/core/subtitle_provider.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

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

std::string toLower(const std::string& s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
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
      continue;  // .ass / bitmap subs need a decoder, not this parser
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

} // namespace soar
