#pragma once

#include "soar/core/subtitle_provider.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace soar {

// Runs one SubtitleTranslator::translate on a background thread and lets a
// poller watch its progress and collect the result — the substrate of a
// live translation panel, where the wire batches (a chat round trip per
// batch, up to config_.timeout each) would otherwise freeze a UI thread.
//
// One session is reusable: start() again after a finished translation
// reaps the old thread and clears the result. A second start() while one
// is still running returns false, so a caller that wants to keep only the
// newest translation can just call start() once per user action.
//
// All observation is thread-safe and non-blocking for the poller:
// running() and progress() read atomics, takeResult() moves a finished
// result out under a mutex exactly once. The result itself is
// all-or-nothing — see SubtitleTranslator::translate — so a session that
// reports a failed translation delivers no partial track. The worker is
// joined on destruction, so a session is always safe to drop.
class SubtitleTranslateSession {
 public:
  SubtitleTranslateSession() = default;
  ~SubtitleTranslateSession();
  SubtitleTranslateSession(const SubtitleTranslateSession&) = delete;
  SubtitleTranslateSession& operator=(const SubtitleTranslateSession&) = delete;

  // Starts a translation of `subtitle_text` into `target_language` with
  // `config`. `progress`, when set, is called on the worker after each
  // batch (see TranslateProgress). A non-empty `source_language` is named
  // in the system prompt (the panel's "from" selector); empty keeps the
  // auto-detect phrasing. Returns false when a translation is already
  // running (the caller keeps the current one).
  bool start(SubtitleTranslateConfig config, std::string subtitle_text,
             std::string target_language, TranslateProgress progress = {},
             std::string source_language = {});

  // True while the worker is alive.
  bool running() const { return running_.load(std::memory_order_acquire); }

  // Current {done, total} cue counts. Before start() both are 0; after the
  // first progress report `total` is the cue count and stays constant.
  std::pair<size_t, size_t> progress() const {
    return {done_.load(std::memory_order_acquire),
            total_.load(std::memory_order_acquire)};
  }

  // Consumes a finished result exactly once. Returns true and moves the
  // outcome into `ok`/`out`/`err` (each may be null) when the translation
  // has finished; false before that or after the result was taken. A false
  // `*ok` is a failure with a reason in `*err`.
  bool takeResult(bool* ok, std::string* out, std::string* err);

  // Blocks until the worker exits (no-op when nothing is running).
  void join();

 private:
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<size_t> done_{0};
  std::atomic<size_t> total_{0};
  std::mutex result_mu_;
  bool finished_ = false;  // guarded by result_mu_
  bool ok_ = false;        // guarded by result_mu_
  std::string out_;        // guarded by result_mu_
  std::string err_;        // guarded by result_mu_
};

}  // namespace soar
