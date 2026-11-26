#include "soar/core/subtitle_translate_session.h"

namespace soar {

SubtitleTranslateSession::~SubtitleTranslateSession() { join(); }

bool SubtitleTranslateSession::start(SubtitleTranslateConfig config,
                                     std::string subtitle_text,
                                     std::string target_language,
                                     TranslateProgress progress,
                                     std::string source_language) {
  if (running_.load(std::memory_order_acquire)) {
    return false;  // one translation at a time; the caller keeps this one
  }
  if (thread_.joinable()) {
    // A finished previous translation: reap it, then start over with a
    // cleared result slot.
    thread_.join();
    std::lock_guard<std::mutex> lock(result_mu_);
    finished_ = false;
    ok_ = false;
    out_.clear();
    err_.clear();
    done_.store(0, std::memory_order_release);
    total_.store(0, std::memory_order_release);
  }

  // The translator is configured per start(): the session owns the config
  // copy, so a caller that mutates its own settings mid-run cannot reach a
  // running translation.
  SubtitleTranslator translator(config);
  running_.store(true, std::memory_order_release);
  thread_ = std::thread(
      [this, translator = std::move(translator), text = std::move(subtitle_text),
       target = std::move(target_language), progress = std::move(progress),
       source = std::move(source_language)]() mutable {
        std::string out, err;
        const bool ok = translator.translate(
            text, target, &out, &err,
            [this, &progress](size_t done, size_t total,
                              const std::vector<std::string>& originals,
                              const std::vector<std::string>& translated) {
              done_.store(done, std::memory_order_release);
              total_.store(total, std::memory_order_release);
              if (progress) progress(done, total, originals, translated);
            },
            source);
        {
          std::lock_guard<std::mutex> lock(result_mu_);
          finished_ = true;
          ok_ = ok;
          out_ = std::move(out);
          err_ = std::move(err);
        }
        // Set after finished_ so a poller that observes running()==false
        // can rely on takeResult() finding the result.
        running_.store(false, std::memory_order_release);
      });
  return true;
}

bool SubtitleTranslateSession::takeResult(bool* ok, std::string* out,
                                          std::string* err) {
  std::lock_guard<std::mutex> lock(result_mu_);
  if (!finished_) return false;
  finished_ = false;  // consumed once
  if (ok != nullptr) *ok = ok_;
  if (out != nullptr) *out = std::move(out_);
  if (err != nullptr) *err = std::move(err_);
  return true;
}

void SubtitleTranslateSession::join() {
  if (thread_.joinable()) thread_.join();
}

}  // namespace soar
