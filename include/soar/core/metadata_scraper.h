#pragma once

#include <chrono>
#include <string>

namespace soar {

// What a network scrape found. These are the same two fields the offline NFO
// sidecar carries (docs/mvp.md §3), so a scrape and a sidecar both land in
// the library's one NfoInfo: the row's display title and the text behind the
// hover tooltip. Posters, ratings, seasons and cast are the next batch's
// concern - the library's seam is these two strings, and a scraper that
// returned more would have nowhere to put it.
struct ScrapedMetadata {
  std::string title;
  std::string plot;
};

// Endpoint settings for HttpMetadataScraper. `endpoint` empty means the
// scraper is offline: scrape() returns false immediately without any network
// activity. Only http:// endpoints are honored (a https:// endpoint degrades
// the same way - TLS is deliberately out of scope for this codebase, see
// http_cache.h); the key, when set, travels as the X-API-Key request header
// and is never logged or embedded anywhere in the repo.
struct HttpMetadataConfig {
  std::string endpoint;
  std::string api_key;
  std::chrono::milliseconds timeout{5000};
};

// Network metadata scrape (docs/mvp.md §3, the network half of "媒体库与
// 刮削"). The offline half - the <name>.nfo sidecar next to the media file -
// is the library's own reader; this class is what the library asks when a
// file has no sidecar and an endpoint is configured.
//
// The configuration follows the external subtitle provider: the endpoint and
// key arrive from the embedding application (the window reads them from the
// environment), the core embeds no credentials and no built-in vendor
// endpoints, and an unconfigured scraper touches no network.
//
// Wire protocol (TMDB-shaped, so a service can implement it; tests drive it
// with a local fixture server):
//   search  GET {endpoint}/search/movie?query={percent-encoded title}
//           X-API-Key: {api_key}            (header sent when configured)
//           200 + JSON, one entry per hit:
//           {"results":[
//              {"title":"...", "overview":"...",
//               "release_date":"2001-09-07", "poster_path":"/abc.jpg",
//               "vote_average":7.4}, ...]}
//   "title" is the only field the scraper requires; "overview" maps to plot
//   and everything else is ignored by this batch. An empty result list, an
//   answer with no "title", a 404 or any other failure returns false.
//
// Everything degrades to "no metadata": the media keeps playing and the
// library row falls back to the file name.
class HttpMetadataScraper {
 public:
  HttpMetadataScraper() = default;
  explicit HttpMetadataScraper(HttpMetadataConfig config);

  // Re-configures after construction (the window reads its settings from
  // the environment into this).
  void configure(HttpMetadataConfig config);

  // True when an http:// endpoint is configured - the caller's signal that a
  // scrape is worth attempting at all.
  bool online() const;

  // Scrapes the first hit for `query`. On success fills `*out` and returns
  // true; an answer without a non-empty "title" is a miss, not a failure to
  // distinguish. On failure returns false, leaves `*out` untouched and puts
  // a short reason into `*err` - either pointer may be null.
  bool scrape(const std::string& query, ScrapedMetadata* out,
              std::string* err) const;

 private:
  HttpMetadataConfig config_;
};

}  // namespace soar
