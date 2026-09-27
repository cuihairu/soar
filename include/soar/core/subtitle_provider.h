#pragma once

#include "soar/core/backend.h"
#include "soar/core/subtitle_text.h"

#include <chrono>
#include <string>
#include <vector>

namespace soar {

// One subtitle source a provider found for a media item.
struct SubtitleCandidate {
  // A local path for the sidecar provider; a URL or a provider-specific
  // handle for a remote one.
  std::string path;
  // Best-effort language tag as it appears in the file name ("en",
  // "zh-Hans"); empty when the name carries none.
  std::string language;
  // Label for the UI, i.e. the file name as it sits on disk.
  std::string title;
  SubtitleFormat format{SubtitleFormat::Unknown};
};

// Pluggable subtitle source (docs/mvp.md §6 "字幕下载": the interface lives
// in the core, the implementation can be swapped — the local sidecar today,
// an OpenSubtitles-style HTTP client later — without the player noticing).
//
// Contract for every implementation:
//  - findCandidates() must never throw and must not be allowed to hold up
//    playback: a source that is unreachable, empty or misconfigured simply
//    returns no candidates, which the UI reports as "no external subtitles";
//  - fetch() returns false instead of throwing when the bytes cannot be
//    read, and the candidate is then not offered.
// Discovery is a convenience layered on top of playback, so every failure
// mode degrades to "the media plays, without that subtitle".
class SubtitleProvider {
public:
  virtual ~SubtitleProvider() = default;

  virtual std::vector<SubtitleCandidate> findCandidates(
      const MediaSource& source) const = 0;

  virtual bool fetch(const SubtitleCandidate& candidate, std::string& out) const = 0;
};

// Local sidecar files sitting next to the media. For /videos/movie.mkv it
// offers movie.srt, movie.en.srt, movie.zh-Hans.vtt, ... — the naming
// players have always agreed on. Only the plain-text formats the core
// parser reads are offered (.srt / .vtt / .webvtt); bitmap subtitles
// (PGS, VobSub) need a decoder and are skipped.
//
// Matching is case-insensitive, an untagged file (movie.srt) sorts ahead of
// tagged ones, and the rest is ordered by name so the menu does not shuffle
// between polls. A remote source (http/https) yields nothing: a sidecar is
// a property of a local directory.
class SidecarSubtitleProvider : public SubtitleProvider {
public:
  std::vector<SubtitleCandidate> findCandidates(
      const MediaSource& source) const override;

  bool fetch(const SubtitleCandidate& candidate, std::string& out) const override;
};

// Endpoint settings for ExternalSubtitleProvider. `endpoint` empty means
// the provider is offline: findCandidates() returns no candidates and
// fetch() returns false without any network activity. Only http://
// endpoints are honored (a https:// endpoint degrades the same way — TLS
// is deliberately out of scope for this codebase, see http_cache.h); the
// key, when set, travels as the X-API-Key request header and is never
// logged or embedded anywhere in the repo.
struct HttpSubtitleConfig {
  std::string endpoint;
  std::string api_key;
  std::chrono::milliseconds timeout{5000};
};

// External subtitle provider for remote services (docs/mvp.md §6 "字幕下载").
// A plain HTTP client over a small documented protocol, fully driven by
// configuration: an unconfigured provider (empty endpoint) never touches
// the network and answers no candidates, and the endpoint/key arrive from
// the embedding application — the core embeds no credentials and no
// built-in vendor endpoints.
//
// Wire protocol (implemented by the configured service; tests drive it
// with a local fixture server):
//   search  GET {endpoint}?size={bytes}&hash={hex16}&name={stem}
//           X-API-Key: {api_key}            (header sent when configured)
//           200 + text body, one candidate per line, four tab-separated
//           fields: url, language, title, extension ("srt"/"vtt"). Lines
//           starting with '#' are comments; an empty body means no
//           candidates. Only http:// candidate urls are offered (this
//           client speaks no TLS, like HttpCache — see http_cache.h).
//   fetch   GET {candidate url}; 200 + a body that detectSubtitleFormat()
//           recognizes (SubRip or WebVTT) yields the text, anything else
//           (non-2xx, wrong bytes, unreachable) yields false.
//
// The hash is the widely used sum-of-64-bit-words recipe over the first
// and last 64 KiB plus the file size (see mediaHashHex), so a service can
// compute it independently; the file name rides along as "name".
//
// Every failure mode — unconfigured, unreachable, timeout, non-2xx,
// unparsable answer, unreadable media — degrades to no candidates or a
// false fetch, per the SubtitleProvider contract: the media keeps
// playing, without that subtitle.
class ExternalSubtitleProvider : public SubtitleProvider {
 public:
  ExternalSubtitleProvider() = default;
  explicit ExternalSubtitleProvider(HttpSubtitleConfig config);

  // Re-configures after construction (the window reads its settings from
  // the environment into this).
  void configure(HttpSubtitleConfig config);

  std::vector<SubtitleCandidate> findCandidates(
      const MediaSource& source) const override;

  bool fetch(const SubtitleCandidate& candidate, std::string& out) const override;

 private:
  HttpSubtitleConfig config_;
};

// Persists fetched subtitle text under <dir>/subtitles/ (or the system
// temp directory when `dir` is empty), named after a sanitized copy of the
// candidate title — path separators and other hostile characters become
// underscores — with a SubRip/WebVTT extension appended when the title
// carries none. Returns the written path, or "" when the directory cannot
// be created or the file cannot be written. A returned path is loadable by
// Player::loadExternalSubtitle, which is how a download joins the sidecar
// pipeline.
std::string storeExternalSubtitle(const std::string& dir,
                                  const SubtitleCandidate& candidate,
                                  const std::string& text);

// The media digest the search query carries ("hash="): 16 lowercase hex
// digits — the sum of the unsigned 64-bit little-endian words of the first
// and last 64 KiB of the file plus its size in bytes. Shorter files
// contribute whatever they hold (head and tail may then overlap). Returns
// "" when the path is not a readable regular file.
std::string mediaHashHex(const std::string& media_path);

} // namespace soar
