#pragma once

#include "soar/core/backend.h"
#include "soar/core/subtitle_text.h"

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

// External subtitle provider for remote services (OpenSubtitles, etc.).
// findCandidates returns no candidates when the service is unreachable —
// the UI falls back to "no external subtitles". fetch() returns false
// when the bytes cannot be read, and the candidate is then not offered.
// The concrete HTTP client is left to a user-provided translation of this
// interface; the core embeds no API keys and makes no network requests.
class ExternalSubtitleProvider : public SubtitleProvider {
public:
  std::vector<SubtitleCandidate> findCandidates(
      const MediaSource& source) const override {
    // No network in the core: a remote provider always reports empty.
    return {};
  }

  bool fetch(const SubtitleCandidate& candidate, std::string& out) const override {
    // Pure virtual: concrete HTTP implementation provided by the user.
    // Returns false so the candidate is silently dropped.
    (void)candidate;
    (void)out;
    return false;
  }
};

} // namespace soar
