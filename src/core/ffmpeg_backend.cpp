#include "soar/core/ffmpeg_backend.h"

#include <fmt/format.h>

#include "soar/core/ass_dialogue.h"
#include "soar/core/http_cache.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/time.h>
#include <libavutil/timestamp.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string_view>

#ifdef SOAR_WITH_SDL2
#  include <SDL.h>
#endif

namespace soar {
namespace {

constexpr std::size_t kMaxVideoQueueFrames = 6;
constexpr std::size_t kMaxAudioQueueFrames = 32;
constexpr std::size_t kMaxSubtitleQueueFrames = 4;
constexpr auto kPositionEmitGranularity = std::chrono::milliseconds(200);
// Network stall watchdog: a read that stays quiet longer than
// kNetworkStallReportMs reports BufferingStarted, and only a stall past
// kNetworkStallLimitMs aborts the read into Error. The abort deliberately
// waits out the tolerance window because killing a read kills the
// connection; a merely slow source recovers on the same socket.
constexpr auto kNetworkStallReportMs = std::chrono::milliseconds(10'000);
constexpr auto kNetworkStallLimitMs = std::chrono::milliseconds(60'000);

std::string dictValue(AVDictionary* dict, const char* key) {
  if (!dict || !key) {
    return {};
  }
  const AVDictionaryEntry* entry = av_dict_get(dict, key, nullptr, 0);
  if (!entry || !entry->value) {
    return {};
  }
  return std::string(entry->value);
}

std::string codecNameFromCodecId(AVCodecID codec_id) {
  const AVCodec* codec = avcodec_find_decoder(codec_id);
  if (codec && codec->name) {
    return std::string(codec->name);
  }
  return fmt::format("codec({})", static_cast<int>(codec_id));
}

// ASS event text extraction. An ASS rect payload is a script section whose
// event lines read "Dialogue: layer,start,end,style,name,mL,mR,mV,effect,
// text" — the human payload starts after the ninth comma. Override blocks
// {...} are dropped and hard breaks (\N, \n) become spaces, so an SRT line
// like "Hello" surfaces as plain "Hello" (the SRT/WebVTT decoders emit
// ASS-format rects; see processSubtitleFrame).
// The Dialogue-line reassembly (assTimestamp/assDialogueLine) lives in
// ass_dialogue.{h,cpp} as pure functions so the tolerant-parsing arms are
// unit-testable headlessly (docs/mvp.md §6).

// The ff_ass_get_dialog decoder wraps synthesized events in brackets:
// "[readorder,layer,style,speaker,mL,mR,mV,effect,text]" — 8 commas, 9 fields.
std::string assDialogueText(const char* ass) {
  if (ass == nullptr) {
    return {};
  }
  std::string_view s(ass);
  // Strip surrounding brackets if present (ff_ass_get_dialog format).
  // ff_ass_get_dialog emits 8‑comma text like [readorder,layer,style,speaker,
  // 0,0,0,,text] — the surrounding [ ] are stripped so the payload after the
  // eighth comma is the plain text.  The ASS decoder passes full
  // "Dialogue:..." lines through without surrounding brackets, and SRT/WebVTT
  // synthesizers produce 8‑comma text without brackets; this code is a defensive
  // no‑op for those paths but handles the occasional bracketed wrapper.
  if (!s.empty()) {
    if (s.front() == '[') {
      s.remove_prefix(1);
    }
    if (!s.empty() && s.back() == ']') {
      s.remove_suffix(1);
    }
  }
  std::string out;
  std::size_t pos = 0;
  while (pos < s.size()) {
    const std::size_t eol = s.find('\n', pos);
    const std::size_t line_end = eol == std::string_view::npos ? s.size() : eol;
    std::string_view line = s.substr(pos, line_end - pos);
    pos = eol == std::string_view::npos ? s.size() : eol + 1;
    const bool full_event = line.rfind("Dialogue:", 0) == 0;
    const int commas_needed = full_event ? 9 : 8;
    int commas = 0;
    std::size_t i = 0;
    while (i < line.size() && commas < commas_needed) {
      if (line[i] == ',') {
        ++commas;
      }
      ++i;
    }
    if (commas < commas_needed) {
      continue;
    }
    bool in_braces = false;
    for (; i < line.size(); ++i) {
      const char c = line[i];
      if (in_braces) {
        if (c == '}') {
          in_braces = false;
        }
        continue;
      }
      if (c == '{') {
        in_braces = true;
        continue;
      }
      if (c == '\\' && i + 1 < line.size() && (line[i + 1] == 'N' || line[i + 1] == 'n')) {
        out.push_back(' ');
        ++i;
        continue;
      }
      // \N at end of line: also convert to space for robust handling
      if (c == '\\' && i + 1 >= line.size()) {
        out.push_back(' ');
        continue;
      }
      out.push_back(c);
    }
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '\r' || out.back() == ']')) {
    out.pop_back();
  }
  return out;
}

int pickBestStreamIndex(AVFormatContext* ctx, AVMediaType type) {
  if (!ctx) {
    return -1;
  }
  int first = -1;
  int best = -1;
  for (unsigned int i = 0; i < ctx->nb_streams; ++i) {
    AVStream* stream = ctx->streams[i];
    if (!stream || !stream->codecpar) {
      continue;
    }
    if (stream->codecpar->codec_type != type) {
      continue;
    }
    if (first < 0) {
      first = static_cast<int>(i);
    }
    if ((stream->disposition & AV_DISPOSITION_DEFAULT) != 0) {
      best = static_cast<int>(i);
      break;
    }
  }
  return best >= 0 ? best : first;
}

void freeFrame(AVFrame*& frame) {
  if (!frame) {
    return;
  }
  av_frame_free(&frame);
  frame = nullptr;
}

template <typename Queue>
void clearDecodedQueue(Queue& q) {
  while (!q.empty()) {
    auto item = q.front();
    q.pop();
    freeFrame(item.frame);
  }
}

} // namespace

struct FFmpegBackend::VideoConvertState {
  int width{0};
  int height{0};
  AVPixelFormat src_fmt{AV_PIX_FMT_NONE};
  AVFrame* dst{nullptr};
  std::vector<std::uint8_t> buffer;

  ~VideoConvertState() {
    if (dst) {
      av_frame_free(&dst);
      dst = nullptr;
    }
  }
};

#ifdef SOAR_WITH_SDL2
struct FFmpegBackend::SDLAudio {
  SDL_AudioDeviceID dev{0};
  SDL_AudioSpec obtained{};
  int sample_rate{0};
  int channels{0};
  // Endpoint currently open (or last opened): "" = system default. Part
  // of the reuse check — a device switch must re-open even when the
  // stream parameters did not change.
  std::string device;

  // Everything here is reachable from the decode thread (open, queue,
  // backpressure probe) and from whoever calls stop()/close() — the UI
  // thread — so the struct guards itself. The lock is not contended in
  // practice: a device switch does not close anything, it only changes the
  // name the next ensureOpen() compares against. Mutable: the queue-depth
  // probe is a const read that still has to serialize.
  mutable std::mutex mtx;

  bool ensureOpen(int target_rate, int target_channels, const std::string& device_name, std::string& err) {
    std::lock_guard<std::mutex> lock(mtx);
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
      if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        err = SDL_GetError();
        return false;
      }
    }

    if (dev != 0 && sample_rate == target_rate && channels == target_channels && device == device_name) {
      return true;
    }

    closeLocked();

    SDL_AudioSpec wanted{};
    wanted.freq = target_rate;
    wanted.format = AUDIO_F32SYS;
    wanted.channels = static_cast<Uint8>(std::clamp(target_channels, 1, 8));
    wanted.samples = 2048;
    wanted.callback = nullptr; // queued audio

    dev = SDL_OpenAudioDevice(device_name.empty() ? nullptr : device_name.c_str(), 0, &wanted,
                              &obtained, SDL_AUDIO_ALLOW_FORMAT_CHANGE);
    if (dev == 0) {
      err = SDL_GetError();
      return false;
    }

    sample_rate = obtained.freq;
    channels = obtained.channels;
    device = device_name;
    SDL_PauseAudioDevice(dev, 0);
    return true;
  }

  // There is deliberately no "reopen for another device" entry point: the
  // switch is a name change plus the next ensureOpen() from the decode
  // thread, which re-opens under this same lock. Doing it in the UI
  // thread would mean closing an endpoint while a queue is draining.

  void clear() {
    std::lock_guard<std::mutex> lock(mtx);
    if (dev != 0) {
      SDL_ClearQueuedAudio(dev);
    }
  }

  void close() {
    std::lock_guard<std::mutex> lock(mtx);
    closeLocked();
  }

  void closeLocked() {
    if (dev != 0) {
      SDL_CloseAudioDevice(dev);
      dev = 0;
    }
    sample_rate = 0;
    channels = 0;
    obtained = SDL_AudioSpec{};
    device.clear();
  }

  std::size_t queuedBytes() const {
    std::lock_guard<std::mutex> lock(mtx);
    if (dev == 0) {
      return 0;
    }
    return SDL_GetQueuedAudioSize(dev);
  }

  bool queue(const void* data, std::size_t bytes, std::string& err) {
    std::lock_guard<std::mutex> lock(mtx);
    if (dev == 0) {
      err = "audio device not open";
      return false;
    }
    if (SDL_QueueAudio(dev, data, static_cast<Uint32>(bytes)) != 0) {
      err = SDL_GetError();
      return false;
    }
    return true;
  }
};
#else
struct FFmpegBackend::SDLAudio {
  bool ensureOpen(int, int, const std::string&, std::string&) { return false; }
  void clear() {}
  void close() {}
  std::size_t queuedBytes() const { return 0; }
  bool queue(const void*, std::size_t, std::string&) { return false; }
};
#endif

//=============================================================================
// Constructor / Destructor
//=============================================================================

FFmpegBackend::FFmpegBackend() {
  // FFmpeg 4.x+ doesn't need explicit registration
  // av_register_all() is deprecated and no longer needed
}

FFmpegBackend::~FFmpegBackend() {
  // Detach the event sink before close() emits: the sink is usually owned
  // by whoever is destroying us and may already be destroyed (destroyed
  // members are 'pure virtual method called' waiting to happen).
  setEventSink(nullptr);

  // close() joins the decode thread while holding decode_mutex_ and
  // releases the decoders and context, so nothing is left to stop here.
  close();

  cleanupDecoders();
  closeContext();
}

//=============================================================================
// IBackend implementation - Event System
//=============================================================================

void FFmpegBackend::setEventSink(IEventSink* sink) {
  std::lock_guard<std::mutex> lock(event_mutex_);
  event_sink_ = sink;
}

void FFmpegBackend::emit(const Event& e) {
  IEventSink* sink = nullptr;
  {
    std::lock_guard<std::mutex> lock(event_mutex_);
    sink = event_sink_;
  }
  if (!sink) {
    return;
  }

  PlaybackState state{};
  std::chrono::milliseconds position{};
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    state = playback_state_;
    position = current_position_;
  }

  // One copy here (to stamp the envelope fields) instead of a by-value
  // parameter: a by-value emit paid that copy at every call site, which
  // also blew up the branch-coverage denominator with per-site copy/cleanup
  // arcs (see docs/coverage-notes.md §1 on arc noise).
  Event stamped = e;
  stamped.state = state;
  stamped.position = position;
  sink->onEvent(stamped);
}

bool FFmpegBackend::hasMedia() {
  std::lock_guard<std::mutex> lock(decode_mutex_);
  return format_ctx_ != nullptr;
}

bool FFmpegBackend::fail(std::string message, bool emit_event) {
  // Keep a private copy for the event: reading last_error_ outside the
  // lock races with a concurrent fail() rewriting it (TSan: string buffer
  // delete vs memcpy; on macOS the torn size even caused std::bad_alloc).
  std::string recorded = std::move(message);
  {
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_ = recorded;
  }
  if (emit_event) {
    emit(Event{EventType::Error, PlaybackState::Stopped, std::chrono::milliseconds(0), recorded});
  }
  return false;
}

bool FFmpegBackend::fatal(std::string message, bool emit_event) {
  std::string recorded = std::move(message);
  {
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_ = recorded;
  }

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    playback_state_ = PlaybackState::Error;
  }

  if (emit_event) {
    emit(Event{EventType::Error, PlaybackState::Error, std::chrono::milliseconds(0), recorded});
    emit(Event{EventType::StateChanged});
  }
  return false;
}

//=============================================================================
// IBackend implementation - Media Open/Close
//=============================================================================

bool FFmpegBackend::open(const MediaSource& source) {
  // Close any existing media (does its own locking).
  close();

  // Reset state
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    playback_state_ = PlaybackState::Stopped;
    current_position_ = std::chrono::milliseconds(0);
    last_emitted_position_ = std::chrono::milliseconds(0);
    clock_origin_ = std::chrono::steady_clock::time_point{};
  }

  // Build the whole open path under decode_mutex_: format_ctx_ and the
  // decoders must not be visible to other control calls (pause/stop/seek/
  // selectTrack/close) until the media is fully set up. Lock order is
  // decode_mutex_ -> {state|info|error}_mutex_ everywhere. Events are
  // emitted after the lock is released so that user callbacks may safely
  // re-enter the public API.
  bool ok = false;
  {
    std::lock_guard<std::mutex> media_lock(decode_mutex_);

    // Open the media file
    AVFormatContext* ctx = nullptr;
    if (!openContext(source, &ctx)) {
      format_ctx_ = nullptr;
    } else {
      format_ctx_ = ctx;

      // Find and initialize stream info, then set up decoders
      if (findStreamInfo() && setupDecoders()) {
        // Build MediaInfo
        std::lock_guard<std::mutex> lock(info_mutex_);

        media_info_ = MediaInfo{};

    // Live sources (RTSP in particular) report AV_NOPTS_VALUE-ish negative
    // durations; expose that as "no duration" instead of a garbage negative.
    media_info_.duration = std::chrono::milliseconds(
      format_ctx_->duration < 0
        ? 0
        : static_cast<int64_t>(format_ctx_->duration * 1000.0 / AV_TIME_BASE)
    );
    media_info_.seekable = false;
    if (format_ctx_->pb) {
      media_info_.seekable = (format_ctx_->pb->seekable & AVIO_SEEKABLE_NORMAL) != 0;
    } else {
      media_info_.seekable = (format_ctx_->ctx_flags & AVFMTCTX_UNSEEKABLE) == 0;
    }

    media_info_.selected_video = video_stream_index_;
    media_info_.selected_audio = audio_stream_index_;
    media_info_.selected_subtitle = -1; // default: subtitles off

    for (unsigned int i = 0; i < format_ctx_->nb_streams; ++i) {
      AVStream* stream = format_ctx_->streams[i];
      if (!stream || !stream->codecpar) {
        continue;
      }

      TrackType track_type{};
      std::string fallback_title;
      const auto media_type = stream->codecpar->codec_type;
      if (media_type == AVMEDIA_TYPE_VIDEO) {
        track_type = TrackType::Video;
        fallback_title = "Video";
      } else if (media_type == AVMEDIA_TYPE_AUDIO) {
        track_type = TrackType::Audio;
        fallback_title = "Audio";
      } else if (media_type == AVMEDIA_TYPE_SUBTITLE) {
        track_type = TrackType::Subtitle;
        fallback_title = "Subtitles";
      } else {
        continue;
      }

      std::string codec;
      if (media_type == AVMEDIA_TYPE_VIDEO && static_cast<int>(i) == video_stream_index_ && video_decoder_) {
        codec = getCodecName(video_decoder_);
      } else if (media_type == AVMEDIA_TYPE_AUDIO && static_cast<int>(i) == audio_stream_index_ && audio_decoder_) {
        codec = getCodecName(audio_decoder_);
      } else {
        codec = codecNameFromCodecId(stream->codecpar->codec_id);
      }

      std::string language;
      if (track_type != TrackType::Video) {
        language = dictValue(stream->metadata, "language");
        if (language.empty()) {
          language = "und";
        }
      }

      std::string title = dictValue(stream->metadata, "title");
      if (title.empty()) {
        title = fallback_title;
      }

      const bool is_default = (stream->disposition & AV_DISPOSITION_DEFAULT) != 0;
      media_info_.tracks.push_back({
        static_cast<int>(i),
        track_type,
        std::move(codec),
        std::move(language),
        std::move(title),
        is_default
      });
    }

        // The export pass (exportSubtitleText) reopens this source on its
        // own later; remember what to reopen.
        opened_source_ = source;

        ok = true;
      } else {
        // findStreamInfo or setupDecoders failed: release everything that
        // was created so far; the errors were already recorded (without
        // emitting) by the fatal() calls inside.
        cleanupDecoders();
        closeContext();
        format_ctx_ = nullptr;
      }
    }
  }

  if (ok) {
    // Emit events (outside decode_mutex_)
    emit(Event{EventType::MediaInfoChanged});
    emit(Event{EventType::StateChanged});
    return true;
  }

  // The open path failed; fatal() already recorded the error and switched
  // the state to Error. Emit the matching events outside the lock.
  emit(Event{EventType::Error, PlaybackState::Error, std::chrono::milliseconds(0), lastError()});
  emit(Event{EventType::StateChanged});
  return false;
}

void FFmpegBackend::close() {
  {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    if (decode_thread_.joinable()) {
      should_stop_decoding_ = true;
      decode_cv_.notify_all();
      decode_thread_.join();
      should_stop_decoding_ = false;
    }

    // The export pass snapshots this under the same lock; a closed media
    // has no source to reopen.
    opened_source_ = MediaSource{};

    // Drop any audio-track switch that never reached the decode loop.
    {
      std::lock_guard<std::mutex> plock(pending_audio_mutex_);
      if (pending_audio_decoder_) {
        avcodec_free_context(&pending_audio_decoder_);
        pending_audio_decoder_ = nullptr;
      }
      pending_audio_track_ = -1;
    }
    // Same for a subtitle-track switch.
    {
      std::lock_guard<std::mutex> plock(pending_subtitle_mutex_);
      if (pending_subtitle_decoder_) {
        avcodec_free_context(&pending_subtitle_decoder_);
        pending_subtitle_decoder_ = nullptr;
      }
      pending_subtitle_track_ = -1;
    }

    cleanupDecoders();
    closeContext();
  }

  if (sdl_audio_) {
    sdl_audio_->close();
  }

  // Clear queues
  {
    std::lock_guard<std::mutex> vlock(video_queue_mutex_);
    clearDecodedQueue(video_queue_);
  }
  {
    std::lock_guard<std::mutex> alock(audio_queue_mutex_);
    clearDecodedQueue(audio_queue_);
  }

  // Reset state
  {
    std::lock_guard<std::mutex> lock(info_mutex_);
    media_info_ = MediaInfo{};
  }

  // current_position_/clock_origin_/last_emitted_position_ are guarded by
  // state_mutex_ everywhere else (position(), decode loop, pause/play/
  // seek/setRate); resetting them here under info_mutex_ raced with
  // position() reading under state_mutex_ (TSan, open/close storm).
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    playback_state_ = PlaybackState::Stopped;
    current_position_ = std::chrono::milliseconds(0);
    clock_origin_ = std::chrono::steady_clock::time_point{};
    last_emitted_position_ = std::chrono::milliseconds(0);
  }

  // close() tears down all playback state, the A-B window with it.
  loop_a_ms_.store(-1, std::memory_order_relaxed);
  loop_b_ms_.store(-1, std::memory_order_relaxed);

  {
    std::lock_guard<std::mutex> lock(video_frame_mutex_);
    video_frame_ready_ = false;
    latest_video_frame_ = DecodedVideoFrame{};
    staging_video_frame_ = DecodedVideoFrame{};
    presented_video_frame_ = DecodedVideoFrame{};
  }

  {
    std::lock_guard<std::mutex> lock(subtitle_frame_mutex_);
    subtitle_frames_ = {};
  }

  // Sidecars belong to the media that was open: the next open gets the ones
  // next to *its* file, so nothing survives a close.
  clearExternalSubtitles();

  emit(Event{EventType::MediaInfoChanged});
  emit(Event{EventType::StateChanged});
}

//=============================================================================
// IBackend implementation - Playback Control
//=============================================================================

bool FFmpegBackend::play() {
  PlaybackState state{};
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    state = playback_state_;
  }

  bool has_media = false;
  {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    has_media = format_ctx_ != nullptr;

    if (has_media) {
      if ((state == PlaybackState::Ended || state == PlaybackState::Error) && decode_thread_.joinable()) {
        decode_thread_.join();
      }

      if (!decode_thread_.joinable()) {
        should_stop_decoding_ = false;
        if (state == PlaybackState::Ended || state == PlaybackState::Error) {
          // Replay after EOF: rewind the demuxer as well as the clock.
          // Without this the new loop resumes reading at the old offset,
          // hits EOF immediately and flips the state back to Ended right
          // after play() set Playing (seek-bounds media test, TSan run
          // 35857173070).
          seek_requested_ = true;
          seek_target_ = 0;
        }
        decode_thread_ = std::thread(&FFmpegBackend::decodeLoop, this);
      }
    }
  }

  if (!has_media) {
    return fail("play: no media opened");
  }

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (playback_state_ == PlaybackState::Playing) {
      return true;
    }

    if (playback_state_ == PlaybackState::Ended) {
      current_position_ = std::chrono::milliseconds(0);
      last_emitted_position_ = std::chrono::milliseconds(0);
    }

    const auto rate = playback_rate_.load();
    const auto now = std::chrono::steady_clock::now();
    const auto scaled = std::chrono::duration<double, std::milli>(current_position_.count() / rate);
    clock_origin_ = now - std::chrono::duration_cast<std::chrono::steady_clock::duration>(scaled);
    playback_state_ = PlaybackState::Playing;
  }

  decode_cv_.notify_all();
  emit(Event{EventType::StateChanged});
  return true;
}

bool FFmpegBackend::pause() {
  if (!hasMedia()) {
    return fail("pause: no media opened");
  }

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (playback_state_ == PlaybackState::Paused) {
      return true;
    }

    if (playback_state_ == PlaybackState::Playing) {
      const auto rate = playback_rate_.load();
      const auto now = std::chrono::steady_clock::now();
      const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::duration_cast<std::chrono::milliseconds>(now - clock_origin_).count());
      current_position_ = std::chrono::milliseconds(static_cast<int64_t>(elapsed.count() * rate));
    }

    playback_state_ = PlaybackState::Paused;
  }

  decode_cv_.notify_all();
  emit(Event{EventType::StateChanged});
  return true;
}

bool FFmpegBackend::stop() {
  if (!hasMedia()) {
    return fail("stop: no media opened");
  }

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (playback_state_ == PlaybackState::Stopped) {
      return true;
    }
    playback_state_ = PlaybackState::Stopped;
  }

  {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    if (decode_thread_.joinable()) {
      should_stop_decoding_ = true;
      decode_cv_.notify_all();
      decode_thread_.join();
      should_stop_decoding_ = false;
    }
    // Flush the decoders under this lock: close() and the open() failure
    // path free them under decode_mutex_, so flushing outside the lock
    // dereferenced freed contexts when a concurrent close() won the race
    // (ASan run 35858058702, open/close storm: avcodec_flush_buffers on
    // freed memory).
    flushDecoders();

    // Rewind the demuxer to the start as well: stop() means "back to the
    // beginning" (matching NullBackend). Without this, the next play()
    // would resume reading at the old offset and jump the position back
    // into the middle of the media.
    (void)seekToTimestamp(std::chrono::milliseconds(0), /*emit_event=*/false);
  }

  if (sdl_audio_) {
    sdl_audio_->close();
  }

  {
    std::scoped_lock lock(video_queue_mutex_, audio_queue_mutex_);
    clearDecodedQueue(video_queue_);
    clearDecodedQueue(audio_queue_);
  }

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    current_position_ = std::chrono::milliseconds(0);
    last_emitted_position_ = std::chrono::milliseconds(0);
    clock_origin_ = std::chrono::steady_clock::time_point{};
  }

  emit(Event{EventType::StateChanged});
  emit(Event{EventType::PositionChanged});
  return true;
}

bool FFmpegBackend::seek(std::chrono::milliseconds position) {
  PlaybackState state{};
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    state = playback_state_;
  }

  bool seekable = false;
  std::chrono::milliseconds duration{};
  {
    std::lock_guard<std::mutex> lock(info_mutex_);
    seekable = media_info_.seekable;
    duration = media_info_.duration;
  }
  if (!seekable) {
    return fail("seek: media is not seekable");
  }
  // A live source can be flagged seekable by its AVIO layer yet carry no
  // duration at all; clamping against a zero duration would be UB, and
  // seeking inside a stream with no duration has no meaningful target.
  if (duration <= std::chrono::milliseconds(0)) {
    return fail("seek: media has no duration (live stream)");
  }

  // Clamp position to valid range
  std::chrono::milliseconds clamped = std::clamp(
    position,
    std::chrono::milliseconds(0),
    duration
  );

  // A manual seek disarms the A-B loop: the user moved outside the window
  // on purpose, so the loop must not yank them back (mpv semantics). The
  // decode-thread wrap path never goes through here.
  loop_a_ms_.store(-1, std::memory_order_relaxed);
  loop_b_ms_.store(-1, std::memory_order_relaxed);

  // The whole seek decision and, when no decode thread is running, the
  // actual seekToTimestamp() must stay under decode_mutex_: it protects
  // format_ctx_ against concurrent open/close and prevents a play() from
  // starting a decode thread in the middle of our seek.
  bool handled_here = false;
  bool seek_failed = false;
  bool state_changed = false;
  bool had_media = true;
  {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    if (format_ctx_ == nullptr) {
      had_media = false;
    } else {
      if ((state == PlaybackState::Ended || state == PlaybackState::Error) && decode_thread_.joinable()) {
        decode_thread_.join();
      }
      const bool in_decode_thread =
        decode_thread_.joinable() && (state == PlaybackState::Playing || state == PlaybackState::Paused);

      if (in_decode_thread) {
        // Store seek request for decode thread
        seek_requested_ = true;
        seek_target_ = clamped.count();
        decode_cv_.notify_all();
      } else if (seekToTimestamp(clamped, /*emit_event=*/false)) {
        {
          std::lock_guard<std::mutex> slock(state_mutex_);
          current_position_ = clamped;
          last_emitted_position_ = clamped;
          const auto rate = playback_rate_.load();
          const auto now = std::chrono::steady_clock::now();
          const auto scaled = std::chrono::duration<double, std::milli>(clamped.count() / rate);
          clock_origin_ = now - std::chrono::duration_cast<std::chrono::steady_clock::duration>(scaled);

          if (clamped == duration && playback_state_ != PlaybackState::Ended) {
            playback_state_ = PlaybackState::Ended;
            state_changed = true;
          } else if (clamped != duration && playback_state_ == PlaybackState::Ended) {
            playback_state_ = PlaybackState::Paused;
            state_changed = true;
          }
        }
        handled_here = true;
      } else {
        // seekToTimestamp failed; the error is recorded (not emitted).
        seek_failed = true;
      }
    }
  }

  if (!had_media) {
    return fail("seek: no media opened");
  }

  if (seek_failed) {
    emit(Event{EventType::Error, PlaybackState::Error, std::chrono::milliseconds(0), lastError()});
    return false;
  }

  if (handled_here) {
    emit(Event{EventType::PositionChanged});
    if (state_changed) {
      emit(Event{EventType::StateChanged});
    }
  }
  return true;
}

bool FFmpegBackend::setRate(double rate) {
  if (rate <= 0.0) {
    return fail("setRate: rate must be positive");
  }

  bool was_playing = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    was_playing = playback_state_ == PlaybackState::Playing;
    if (was_playing) {
      const auto now = std::chrono::steady_clock::now();
      const auto old_rate = playback_rate_.load();
      const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::duration_cast<std::chrono::milliseconds>(now - clock_origin_).count());
      current_position_ = std::chrono::milliseconds(static_cast<int64_t>(elapsed.count() * old_rate));
    }
  }

  playback_rate_ = rate;

  if (was_playing) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const auto now = std::chrono::steady_clock::now();
    const auto scaled = std::chrono::duration<double, std::milli>(current_position_.count() / rate);
    clock_origin_ = now - std::chrono::duration_cast<std::chrono::steady_clock::duration>(scaled);
    decode_cv_.notify_all();
  }

  return true;
}

bool FFmpegBackend::setVolume(double volume01) {
  volume_ = std::clamp(volume01, 0.0, 1.0);
  return true;
}

bool FFmpegBackend::setMuted(bool muted) {
  muted_ = muted;
  return true;
}

//=============================================================================
// IBackend implementation - Media Info
//=============================================================================

MediaInfo FFmpegBackend::mediaInfo() const {
  std::lock_guard<std::mutex> lock(info_mutex_);
  return media_info_;
}

std::chrono::milliseconds FFmpegBackend::position() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return current_position_;
}

PlaybackState FFmpegBackend::state() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return playback_state_;
}

std::string FFmpegBackend::lastError() const {
  std::lock_guard<std::mutex> lock(error_mutex_);
  return last_error_;
}

bool FFmpegBackend::tryGetVideoFrame(DecodedVideoFrame& out) {
  std::lock_guard<std::mutex> lock(video_frame_mutex_);
  if (!video_frame_ready_) {
    return false;
  }
  std::swap(out, latest_video_frame_);
  video_frame_ready_ = false;
  return true;
}

bool FFmpegBackend::tryGetSubtitleFrame(DecodedSubtitleFrame& out) {
  // A sidecar track has no decoder thread to queue frames, so this pull is
  // what advances it: queue whatever the playhead has reached. Embedded
  // tracks keep coming from the decode thread, untouched by this.
  pumpExternalCues(position());
  std::lock_guard<std::mutex> lock(subtitle_frame_mutex_);
  if (subtitle_frames_.empty()) {
    return false;
  }
  out = std::move(subtitle_frames_.front());
  subtitle_frames_.pop();
  return true;
}

// External subtitles: nothing decodes a sidecar, so the cue list parsed at
// load time is replayed against the playhead here, into the same queue the
// embedded path writes. The UI pulls frames and times them itself, so it
// cannot tell the two apart.
void FFmpegBackend::pumpExternalCues(std::chrono::milliseconds now) {
  std::vector<DecodedSubtitleFrame> batch;
  {
    std::lock_guard<std::mutex> lock(external_mutex_);
    if (active_external_ < 0 ||
        static_cast<std::size_t>(active_external_) >= external_subtitles_.size()) {
      return;
    }
    const std::vector<SubtitleCue>& cues = external_subtitles_[active_external_].cues;
    // A playhead that moved backwards means the cue list has to be walked
    // again: a seek, an A-B wrap or a replay all land here, and none of
    // them goes through the decode loop the embedded path relies on.
    if (now < external_last_pos_) {
      external_cue_pos_ = 0;
    }
    external_last_pos_ = now;

    std::size_t due = external_cue_pos_;
    while (due < cues.size() && cues[due].begin <= now) {
      ++due;
    }
    if (due == external_cue_pos_) {
      return;
    }
    // Only the newest few survive the queue cap anyway, and after a forward
    // seek everything in between is already over — so skip straight to the
    // tail instead of replaying hundreds of stale cues.
    const std::size_t first =
      due > kMaxSubtitleQueueFrames ? due - kMaxSubtitleQueueFrames : 0;
    for (std::size_t i = first < external_cue_pos_ ? external_cue_pos_ : first; i < due; ++i) {
      batch.push_back(DecodedSubtitleFrame{cues[i].text, cues[i].begin,
                                           cues[i].end - cues[i].begin});
    }
    external_cue_pos_ = due;
  }
  // Outside the lock: queueSubtitleFrame takes the frame-queue mutex, and
  // external_mutex_ must not be held across it.
  for (const DecodedSubtitleFrame& frame : batch) {
    queueSubtitleFrame(frame.text, frame.pts, frame.duration);
  }
}

void FFmpegBackend::clearExternalSubtitles() {
  std::lock_guard<std::mutex> lock(external_mutex_);
  external_subtitles_.clear();
  active_external_ = -1;
  external_cue_pos_ = 0;
  external_last_pos_ = std::chrono::milliseconds(0);
}

bool FFmpegBackend::saveScreenshot(const std::string& path, bool forceFailEncoder) {
  // Snapshot the retained frame first: the encode below does file IO and
  // must not run under video_frame_mutex_, which the decode thread holds
  // while filling frames.
  DecodedVideoFrame frame;
  {
    std::lock_guard<std::mutex> lock(video_frame_mutex_);
    if (presented_video_frame_.width <= 0 || presented_video_frame_.height <= 0) {
      // Nothing presented yet: never played, audio-only media, or a
      // closed backend. Recorded without an event (see the header).
      return fail("screenshot: no video frame has been presented", false);
    }
    frame = presented_video_frame_;
  }

  std::string error;
  const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_PNG);
  // No null-encoder special case: avcodec_alloc_context3 tolerates a null
  // codec, and avcodec_open2 refuses the resulting context below, so a
  // missing PNG encoder still fails cleanly through the open arm.
  AVCodecContext* ctx = avcodec_alloc_context3(encoder);
  AVFrame* yuv = av_frame_alloc();
  AVFrame* rgb = av_frame_alloc();
  AVPacket* packet = av_packet_alloc();
  SwsContext* sws = nullptr;

  if (!ctx || !yuv || !rgb || !packet) {
    error = "screenshot: PNG encoder unavailable or out of memory";
  } else {
    ctx->width = frame.width;
    ctx->height = frame.height;
    ctx->pix_fmt = forceFailEncoder ? AV_PIX_FMT_NONE : AV_PIX_FMT_RGB24;
    ctx->time_base = AVRational{1, 1};
    if (avcodec_open2(ctx, encoder, nullptr) < 0) {
      error = "screenshot: PNG encoder could not be opened";
    }
  }

  // Wrap the retained YUV420P planes (contiguous, stride == width,
  // normalized by renderVideoFrame) in an AVFrame and convert to the
  // RGB24 the PNG encoder takes. The conversion context is per-call:
  // video_scaler_/video_convert_ belong to the decode thread.
  if (error.empty()) {
    yuv->format = AV_PIX_FMT_YUV420P;
    yuv->width = frame.width;
    yuv->height = frame.height;
    yuv->data[0] = frame.y.data();
    yuv->linesize[0] = frame.stride_y;
    yuv->data[1] = frame.u.data();
    yuv->linesize[1] = frame.stride_u;
    yuv->data[2] = frame.v.data();
    yuv->linesize[2] = frame.stride_v;

    rgb->format = AV_PIX_FMT_RGB24;
    rgb->width = frame.width;
    rgb->height = frame.height;
    if (av_frame_get_buffer(rgb, 32) < 0) {
      error = "screenshot: RGB frame allocation failed";
    } else {
      sws = sws_getContext(frame.width, frame.height, AV_PIX_FMT_YUV420P,
                           frame.width, frame.height, AV_PIX_FMT_RGB24,
                           SWS_BILINEAR, nullptr, nullptr, nullptr);
      if (!sws) {
        error = "screenshot: color conversion setup failed";
      } else if (sws_scale(sws, yuv->data, yuv->linesize, 0, frame.height,
                           rgb->data, rgb->linesize) <= 0) {
        error = "screenshot: color conversion failed";
      }
    }
  }

  if (error.empty()) {
    rgb->pts = 0;
    if (avcodec_send_frame(ctx, rgb) < 0) {
      error = "screenshot: PNG encode failed";
    } else {
      // PNG is intra-only: the drained packet(s) are the whole image.
      FILE* out = std::fopen(path.c_str(), "wb");
      if (!out) {
        error = "screenshot: cannot write " + path;
      } else {
        std::size_t expected = 0;
        std::size_t written = 0;
        while (avcodec_receive_packet(ctx, packet) == 0) {
          expected += static_cast<std::size_t>(packet->size);
          written += std::fwrite(packet->data, 1,
                                 static_cast<std::size_t>(packet->size), out);
          av_packet_unref(packet);
        }
        const bool close_ok = std::fclose(out) == 0;
        if (!close_ok || expected == 0 || written != expected) {
          error = "screenshot: write failed for " + path;
        }
      }
    }
  }

  sws_freeContext(sws);
  av_packet_free(&packet);
  av_frame_free(&rgb);
  av_frame_free(&yuv);
  avcodec_free_context(&ctx);
  if (!error.empty()) {
    return fail(std::move(error), false);
  }
  return true;
}

//=============================================================================
// IBackend implementation - Track Selection
//=============================================================================

bool FFmpegBackend::selectTrack(TrackType type, TrackId id) {
  PlaybackState state{};
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    state = playback_state_;
  }

  int nb_streams = 0;
  {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    if (!format_ctx_) {
      return fail("selectTrack: no media opened");
    }
    nb_streams = static_cast<int>(format_ctx_->nb_streams);
  }

  if (type == TrackType::Video) {
    return fail("selectTrack: video track switching not supported");
  }

  // Subtitle selection is metadata only (no subtitle decoder yet), so it is
  // safe in any state.
  if (type == TrackType::Subtitle) {
    // External tracks (docs/mvp.md §6) live past the container's stream
    // range, so the embedded check below only sees ids below that count.
    int external_slot = -1;
    {
      std::lock_guard<std::mutex> lock(external_mutex_);
      for (std::size_t i = 0; i < external_subtitles_.size(); ++i) {
        if (id == static_cast<TrackId>(nb_streams + static_cast<int>(i))) {
          external_slot = static_cast<int>(i);
          break;
        }
      }
    }

    std::string error;
    {
      std::lock_guard<std::mutex> lock(decode_mutex_);
      if (external_slot < 0 &&
          (id < 0 || id >= static_cast<TrackId>(format_ctx_->nb_streams))) {
        error = "selectTrack: unknown subtitle track id";
      } else if (external_slot < 0 &&
                 (!format_ctx_->streams[id] || !format_ctx_->streams[id]->codecpar ||
                  format_ctx_->streams[id]->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE)) {
        error = "selectTrack: unknown subtitle track id";
      }
    }

    if (!error.empty()) {
      return fail(std::move(error));
    }

    {
      // Arm the pump from the top: the new track's cue 0 may already be
      // behind the playhead, and a seek away from it re-arms it again.
      std::lock_guard<std::mutex> lock(external_mutex_);
      active_external_ = external_slot;
      external_cue_pos_ = 0;
      external_last_pos_ = std::chrono::milliseconds(0);
    }

    // One ASS renderer serves every canvas-drawn subtitle source and
    // follows the last selected subtitle source (docs/mvp.md §6, external
    // documents, batch 1c's synthesized text sidecars, and this batch's
    // selected embedded text streams). Selection is decode semantics too:
    // a sidecar selection closes the embedded decode gate — the
    // batch-1b/1c overlay closes in every build, not just where a canvas
    // can take over — and an embedded selection opens the gate on the
    // selected stream, switching its decoder in when it is not the one
    // open built. Two short critical sections gather the inputs; the
    // renderer calls run outside every backend lock (AssRenderer locks
    // itself, and the decode thread may be feeding or rendering
    // concurrently).
    const bool renderer_live = ass_renderer_.available();
    bool slot_is_document = false;
    std::string document;
    {
      std::lock_guard<std::mutex> lock(external_mutex_);
      if (external_slot >= 0) {
        const ExternalSubtitle& ext =
            external_subtitles_[static_cast<std::size_t>(external_slot)];
        slot_is_document = !ext.document.empty();
        document = ext.document;
      }
    }
    int video_w = 0;
    int video_h = 0;
    bool embedded_ass = false;
    std::string codec_private;
    {
      std::lock_guard<std::mutex> lock(decode_mutex_);
      if (format_ctx_) {
        if (video_stream_index_ >= 0 && format_ctx_->streams[video_stream_index_] &&
            format_ctx_->streams[video_stream_index_]->codecpar) {
          const AVCodecParameters* vpar =
              format_ctx_->streams[video_stream_index_]->codecpar;
          video_w = vpar->width;
          video_h = vpar->height;
        }
        if (external_slot < 0 && id >= 0 &&
            id < static_cast<TrackId>(format_ctx_->nb_streams) &&
            format_ctx_->streams[id] && format_ctx_->streams[id]->codecpar) {
          const AVCodecParameters* par = format_ctx_->streams[id]->codecpar;
          embedded_ass = par->codec_id == AV_CODEC_ID_ASS ||
                         par->codec_id == AV_CODEC_ID_SSA;
          // The stream's own header, for re-arming the feed below. The
          // codecpar stays valid for as long as the media is open, so the
          // default stream's CodecPrivate — what open fed its feed with —
          // is read here again instead of being stashed at open time.
          if (embedded_ass && par->extradata_size > 0 && par->extradata != nullptr) {
            codec_private.assign(reinterpret_cast<const char*>(par->extradata),
                                 static_cast<std::size_t>(par->extradata_size));
          }
        }
      }
    }

    if (external_slot >= 0) {
      // The embedded stream is deselected: subtitle packets stop being
      // processed and whatever the stream already queued is dropped, so
      // from this selection on the sidecar is the only thing arriving in
      // the queue (and on screen) in every build. A packet decoding
      // concurrently with this block can land one final frame; the gate
      // makes it the last.
      subtitle_decode_active_ = false;
      {
        std::lock_guard<std::mutex> lock(subtitle_frame_mutex_);
        std::queue<DecodedSubtitleFrame> empty;
        subtitle_frames_.swap(empty);
      }
      if (renderer_live) {
        // External document, style-faithful: one load replaces the whole
        // track; the canvas follows the playhead from here on. The document
        // is whatever loadExternalSubtitle put in the slot — an .ass/.ssa
        // sidecar's own script, or an SRT/WebVTT sidecar's default-styled
        // resynthesis (batch 1c: the style is ours, the glyphs are
        // libass's). The queue's plain-text copy of the same cues stays the
        // UI's to suppress (subtitleDocumentActive()); the pump itself is
        // not gated, so pulls still serve it in both builds. The feed flag
        // drops first so a packet decoding concurrently cannot append an
        // embedded event into the freshly loaded document track; the
        // document flag then closes the plain-text gate. Between the two
        // stores an event can degrade to one transient plain-text frame —
        // bounded by its own duration, versus an event landing after
        // loadDocument, which would persist on the canvas.
        if (slot_is_document) {
          ass_feed_active_ = false;
          ass_renderer_.loadDocument(document.data(), document.size());
          if (video_w > 0 && video_h > 0) {
            ass_renderer_.setFrameSize(video_w, video_h);
          }
          ass_document_active_ = true;
        }
        // A sidecar without a document needs no renderer work here: in a
        // libass build every loadable sidecar carries one (synthesized at
        // load for text formats, batch 1c), and the no-libass build never
        // armed the renderer in the first place. Whatever the previous
        // selection left standing is the next selection's release to do.
      }
    } else {
      // An embedded subtitle stream: the decode gate opens on it. When it
      // is not the stream open built a decoder for, a fresh decoder is
      // handed over first — the audio switch's contract, so on failure
      // the old decoder stays and the selection fails without touching
      // anything.
      if (id != subtitle_stream_index_) {
        AVCodecContext* new_decoder = nullptr;
        std::string build_error;
        {
          std::lock_guard<std::mutex> lock(decode_mutex_);
          new_decoder = buildSubtitleDecoder(format_ctx_, id, "selectTrack",
                                             build_error);
        }
        if (new_decoder == nullptr) {
          return fail(std::move(build_error));
        }

        // Hand the decoder over. While a decode thread is running it owns
        // subtitle_decoder_, so it must install the new one itself at a
        // packet boundary; otherwise (Stopped / thread already exited) we
        // can swap it in directly under decode_mutex_.
        bool handed_to_decode_thread = false;
        bool thread_stopped = false;
        {
          std::lock_guard<std::mutex> lock(decode_mutex_);
          const bool decode_thread_running =
              decode_thread_.joinable() &&
              (state == PlaybackState::Playing || state == PlaybackState::Paused);

          if (decode_thread_running) {
            std::lock_guard<std::mutex> plock(pending_subtitle_mutex_);
            if (pending_subtitle_decoder_) {
              // The decode loop never saw the previous pending switch
              // (rapid re-switch overwrites the slot); the slot owns the
              // context, so free it here or it leaks.
              avcodec_free_context(&pending_subtitle_decoder_);
            }
            pending_subtitle_decoder_ = new_decoder;
            pending_subtitle_track_ = id;
            handed_to_decode_thread = true;
            // Keep the gate closed until the swap lands on the decode
            // thread: with it open, one old-stream packet could still
            // decode into the freshly armed renderer.
            subtitle_decode_active_ = false;
          } else {
            // No live decode thread owns subtitle_decoder_, but a leftover
            // one (Ended/Error, or a Stopped-state thread still waiting)
            // must be joined so the swap below is properly ordered.
            if (decode_thread_.joinable()) {
              should_stop_decoding_ = true;
              decode_cv_.notify_all();
              decode_thread_.join();
              should_stop_decoding_ = false;
              thread_stopped = true;
              // stop() parity for the Stopped state this join produces:
              // rewind the demuxer as well. play() keys its replay
              // rewind on Ended/Error, and the join path has just left
              // the demuxer at whatever offset the reaped thread died on
              // (EOF, after the post-EOF selection twin). Without the
              // rewind, a play() from that Stopped state resumes reading
              // at the old offset and the fresh decode thread can hit EOF
              // and exit before the caller's first seek is latched — the
              // EOF break drops a seek that lands in that window, and
              // later ones latch for the dead-but-joinable thread (the
              // Playing/Paused check in seek() cannot tell), so playback
              // never revives (tsan runs 36740091819 and 36748233502,
              // REQUIRE(red) in the post-EOF subtitle selection twin).
              (void)seekToTimestamp(std::chrono::milliseconds(0),
                                    /*emit_event=*/false);
            }
            avcodec_free_context(&subtitle_decoder_);
            subtitle_decoder_ = new_decoder;
            subtitle_stream_index_ = id;
            subtitle_decode_active_ = true;
          }
        }

        if (thread_stopped) {
          reportStoppedPlayback();
        }
        if (handed_to_decode_thread) {
          // Wake the decode loop: it applies the pending switch while
          // paused too, and the gate reopens there with the swap.
          decode_cv_.notify_all();
        }
      } else {
        // Re-selecting the stream open already built: no decoder work,
        // just reopen the gate.
        subtitle_decode_active_ = true;
      }

      // Plain-text frames of this same stream queued before the selection
      // are stale the moment the canvas takes over: drop them so the
      // switch is not wearing both faces for one cue's duration.
      {
        std::lock_guard<std::mutex> lock(subtitle_frame_mutex_);
        std::queue<DecodedSubtitleFrame> empty;
        subtitle_frames_.swap(empty);
      }

      if (renderer_live) {
        // The canvas follows the selection onto the selected stream. An
        // ASS/SSA stream re-arms with its own CodecPrivate (script header
        // + styles); a text stream arms with the synthesized default
        // header — batch 1c's contract, now for embedded text tracks too:
        // the default style is ours, the glyphs are libass's, and
        // mov_text's plain rects are rebuilt into Dialogue lines per
        // frame in processSubtitleFrame. Events resume from wherever the
        // decoder's read position is: cues between the open's start and
        // this point stay hidden until a seek backwards makes the decoder
        // rescan them (flushEvents then drops the stale copies). No
        // automatic rescan here — the batch-1b boundary, unchanged.
        if (ass_renderer_.startStream()) {
          if (!codec_private.empty()) {
            ass_renderer_.feedCodecPrivate(codec_private.data(),
                                           codec_private.size());
          } else {
            const std::string header =
                synthesizeAssDocument({}, video_w, video_h);
            ass_renderer_.feedCodecPrivate(header.data(), header.size());
          }
          if (video_w > 0 && video_h > 0) {
            ass_renderer_.setFrameSize(video_w, video_h);
          }
          ass_feed_active_ = true;
        }
        ass_document_active_ = false;
      }
    }

    {
      std::lock_guard<std::mutex> lock(info_mutex_);
      media_info_.selected_subtitle = id;
    }
    emit(Event{EventType::MediaInfoChanged});
    return true;
  }

  // ---- Audio ----
  // Build the new decoder outside the decode thread; it is never shared
  // until ownership is handed over below.
  AVCodecContext* new_decoder = nullptr;
  std::string error;
  {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    if (id < 0 || id >= static_cast<TrackId>(format_ctx_->nb_streams)) {
      error = "selectTrack: unknown audio track id";
    } else if (!format_ctx_->streams[id] || !format_ctx_->streams[id]->codecpar ||
               format_ctx_->streams[id]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
      error = "selectTrack: unknown audio track id";
    } else {
      const AVCodecParameters* codecpar = format_ctx_->streams[id]->codecpar;
      const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
      if (!codec) {
        error = "selectTrack: audio codec not found";
      } else {
        new_decoder = avcodec_alloc_context3(codec);
        if (!new_decoder) {
          error = "selectTrack: failed to allocate audio decoder context";
        } else {
          int ret = avcodec_parameters_to_context(new_decoder, codecpar);
          if (ret < 0) {
            avcodec_free_context(&new_decoder);
            error = fmt::format("selectTrack: failed to copy audio params: {}", avError(ret));
          } else {
            ret = avcodec_open2(new_decoder, codec, nullptr);
            if (ret < 0) {
              avcodec_free_context(&new_decoder);
              error = fmt::format("selectTrack: failed to open audio decoder: {}", avError(ret));
            }
          }
        }
      }
    }
  }

  if (!error.empty()) {
    return fail(std::move(error));
  }

  // Hand the decoder over. While a decode thread is running it owns
  // audio_decoder_, so it must install the new one itself at a packet
  // boundary; otherwise (Stopped / thread already exited) we can swap it
  // in directly under decode_mutex_.
  bool handed_to_decode_thread = false;
  bool thread_stopped = false;
  {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    const bool decode_thread_running =
      decode_thread_.joinable() &&
      (state == PlaybackState::Playing || state == PlaybackState::Paused);

    if (decode_thread_running) {
      std::lock_guard<std::mutex> plock(pending_audio_mutex_);
      if (pending_audio_decoder_) {
        // The decode loop never saw the previous pending switch (rapid
        // re-switch overwrites the slot); the slot owns the context, so
        // free it here or it leaks.
        avcodec_free_context(&pending_audio_decoder_);
      }
      pending_audio_decoder_ = new_decoder;
      pending_audio_track_ = id;
      handed_to_decode_thread = true;
    } else {
      // No live decode thread owns audio_decoder_, but a leftover one
      // (Ended/Error, or a Stopped-state thread still waiting) must be
      // joined so the swap below is properly ordered against the loop.
      if (decode_thread_.joinable()) {
        should_stop_decoding_ = true;
        decode_cv_.notify_all();
        decode_thread_.join();
        should_stop_decoding_ = false;
        thread_stopped = true;
        // stop() parity, same as the subtitle join above: the Stopped
        // state this leaves must come with a demuxer rewound to the
        // start, or a play() from it (no Ended-keyed replay rewind)
        // resumes at the dead thread's offset and can EOF straight
        // into the dead-but-joinable seek-latch void.
        (void)seekToTimestamp(std::chrono::milliseconds(0),
                              /*emit_event=*/false);
      }
      avcodec_free_context(&audio_decoder_);
      audio_decoder_ = new_decoder;
      audio_stream_index_ = id;
      audio_params_.sample_rate = audio_decoder_->sample_rate;
      audio_params_.channels = audio_decoder_->ch_layout.nb_channels;
      audio_params_.channel_layout = audio_decoder_->ch_layout.u.mask;
    }
  }

  if (thread_stopped) {
    // Joining the decode thread means playback is not running anymore,
    // even if the state snapshot raced ahead of the wind-down.
    reportStoppedPlayback();
  }

  if (handed_to_decode_thread) {
    // Wake the decode loop: it also applies the pending switch while paused.
    decode_cv_.notify_all();
  }

  {
    std::lock_guard<std::mutex> lock(info_mutex_);
    media_info_.selected_audio = id;
  }
  emit(Event{EventType::MediaInfoChanged});
  return true;
}

bool FFmpegBackend::disableSubtitles() {
  {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    if (!format_ctx_) {
      return fail("disableSubtitles: no media opened");
    }
  }
  {
    std::lock_guard<std::mutex> lock(info_mutex_);
    media_info_.selected_subtitle = -1;
  }
  {
    // Stop pumping the sidecar: a stale frame would outlive the selection.
    std::lock_guard<std::mutex> lock(external_mutex_);
    active_external_ = -1;
    external_cue_pos_ = 0;
  }
  // "Off" is decode semantics now, not metadata: the embedded stream's
  // packets stop being processed and the frames it already queued are
  // dropped, so the cue on screen disappears at once instead of wearing
  // out its own duration (a packet decoding concurrently with this block
  // can land one final frame; the gate makes it the last).
  subtitle_decode_active_ = false;
  {
    std::lock_guard<std::mutex> lock(subtitle_frame_mutex_);
    std::queue<DecodedSubtitleFrame> empty;
    subtitle_frames_.swap(empty);
  }
  // The renderer follows: a held document (docs/mvp.md §6, external
  // documents and synthesized text sidecars) or an armed embedded feed is
  // released with an empty-track swap so the canvas goes quiet too.
  if (ass_feed_active_.load(std::memory_order_relaxed)) {
    ass_renderer_.startStream();
    ass_feed_active_ = false;
  } else if (ass_document_active_.load(std::memory_order_relaxed)) {
    ass_renderer_.startStream();
    ass_document_active_ = false;
  }
  emit(Event{EventType::MediaInfoChanged});
  return true;
}

//=============================================================================
// External subtitles (docs/mvp.md §6)
//=============================================================================

bool FFmpegBackend::loadExternalSubtitle(const std::string& path, TrackId& out_id) {
  out_id = -1;

  int nb_streams = 0;
  {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    if (!format_ctx_) {
      return fail("loadExternalSubtitle: no media opened", /*emit_event=*/false);
    }
    nb_streams = static_cast<int>(format_ctx_->nb_streams);
  }

  // Read + parse outside every lock: this is file IO and can be slow on a
  // network mount, and nothing here may block the decode thread.
  std::string text;
  if (!readSubtitleFile(path, text)) {
    return fail("loadExternalSubtitle: cannot read '" + path + "'", /*emit_event=*/false);
  }

  // Named the way the embedded tracks are ("subrip", "mov_text"), so the
  // menu reads the same for both. A sidecar's language tag lives in its
  // file name and is left to the provider; the title below carries it.
  const SubtitleFormat format = detectSubtitleFormat(text);
  std::vector<SubtitleCue> cues;
  std::string document;
  const char* codec = "subrip";
  int video_w = 0, video_h = 0;
  {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    if (format_ctx_) {
      if (video_stream_index_ >= 0 && format_ctx_->streams[video_stream_index_] &&
          format_ctx_->streams[video_stream_index_]->codecpar) {
        const AVCodecParameters* vpar = format_ctx_->streams[video_stream_index_]->codecpar;
        video_w = vpar->width;
        video_h = vpar->height;
      }
    }
  }
  if (format == SubtitleFormat::Ass) {
    // An .ass/.ssa sidecar is a complete script, not a cue list. The
    // Dialogue extraction still gates the load — a file without a single
    // usable event line is not a subtitle — and doubles as the no-libass
    // track itself. With libass the document loads whole in selectTrack
    // (styles faithful), and `cues` is dropped: the pump must stay idle
    // or the UI would draw every line twice, once as plain text and once
    // on the libass canvas.
    cues = assDocumentCues(text);
    if (cues.empty()) {
      return fail("loadExternalSubtitle: no cues in '" + path + "'", /*emit_event=*/false);
    }
    codec = "ass";
    if (ass_renderer_.available()) {
      document = std::move(text);
      cues.clear();
    }
  } else if (format == SubtitleFormat::SubRip || format == SubtitleFormat::WebVtt ||
             format == SubtitleFormat::Ttml) {
    // SRT/WebVTT sidecars are plain-text cue lists. When libass is available
    // they are resynthesized as a default-styled ASS document so the canvas
    // renders them with libass's glyphs and default style (batch 1c). The
    // plain-text copy stays in `cues` for the no-libass degrade and for
    // frame-count bookkeeping in the pump. A TTML/DFXP sidecar joins here:
    // it parses to the same plain cues (styles/regions not applied, like
    // every other text sidecar) and rides the same default-styled canvas.
    cues = parseSubtitleText(text);
    if (cues.empty()) {
      return fail("loadExternalSubtitle: no cues in '" + path + "',", /*emit_event=*/false);
    }
    codec = format == SubtitleFormat::WebVtt   ? "webvtt"
            : format == SubtitleFormat::Ttml   ? "ttml"
                                               : "subrip";
    if (ass_renderer_.available()) {
      document = synthesizeAssDocument(cues, video_w, video_h);
    }
  } else {
    // Unknown format — keep the existing behaviour (should not happen after
    // detectSubtitleFormat covers .srt/.vtt/.ass/.ssa/.ttml/.dfxp).
    return fail("loadExternalSubtitle: no cues in '" + path + "',", /*emit_event=*/false);
  }

  TrackId id = -1;
  {
    std::lock_guard<std::mutex> lock(external_mutex_);
    // Same path twice: replace in place and keep the id it already had, so
    // re-picking a sidecar cannot pile up duplicate tracks.
    std::size_t slot = external_subtitles_.size();
    for (std::size_t i = 0; i < external_subtitles_.size(); ++i) {
      if (external_subtitles_[i].path == path) {
        slot = i;
        break;
      }
    }
    // Ids start at the container's stream count, so an external track can
    // never collide with an embedded one.
    id = static_cast<TrackId>(nb_streams + static_cast<int>(slot));
    if (slot == external_subtitles_.size()) {
      external_subtitles_.push_back(ExternalSubtitle{path, {}, {}});
    }
    external_subtitles_[slot].cues = cues;
    external_subtitles_[slot].document = document;
  }

  {
    const TrackInfo track{id,
                          TrackType::Subtitle,
                          codec,
                          "",
                          std::filesystem::path(path).filename().string(),
                          false};
    std::lock_guard<std::mutex> lock(info_mutex_);
    const auto it = std::find_if(
      media_info_.tracks.begin(),
      media_info_.tracks.end(),
      [&](const TrackInfo& t) { return t.id == id; });
    if (it != media_info_.tracks.end()) {
      *it = track;
    } else {
      media_info_.tracks.push_back(track);
    }
  }

  out_id = id;
  emit(Event{EventType::MediaInfoChanged});
  return true;
}

// SubRip timestamp ("00:00:01,000") for the export pass. Negative input
// clamps to zero; hours run past two digits for very long media.
static std::string srtTimestamp(std::chrono::milliseconds ms) {
  if (ms.count() < 0) {
    ms = std::chrono::milliseconds::zero();
  }
  const auto h = std::chrono::duration_cast<std::chrono::hours>(ms);
  const auto m = std::chrono::duration_cast<std::chrono::minutes>(ms - h);
  const auto s = std::chrono::duration_cast<std::chrono::seconds>(ms - h - m);
  const auto frac = ms - h - m - s;
  return fmt::format("{:02d}:{:02d}:{:02d},{:03d}", h.count(), m.count(),
                     s.count(), frac.count());
}

bool FFmpegBackend::exportSubtitleText(TrackId id, std::string& out_srt) {
  out_srt.clear();

  // Snapshot the reopen inputs under decode_mutex_ and run the whole demux
  // pass outside every lock (loadExternalSubtitle's rule: file/network IO
  // must not block the decode thread). The second input belongs to this
  // call alone — openContext()/avio_cache_ stay untouched, so the decode
  // thread's own input is unaffected.
  std::string uri;
  bool embedded_subtitle = false;
  {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    if (!format_ctx_) {
      return fail("exportSubtitleText: no media opened", /*emit_event=*/false);
    }
    embedded_subtitle =
        id >= 0 && id < static_cast<TrackId>(format_ctx_->nb_streams) &&
        format_ctx_->streams[id] && format_ctx_->streams[id]->codecpar &&
        format_ctx_->streams[id]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE;
    uri = opened_source_.uri;
  }
  if (!embedded_subtitle) {
    return fail("exportSubtitleText: not an embedded subtitle track",
                /*emit_event=*/false);
  }

  // Reopen independently (plain avformat_open_input — the pass does not use
  // the disk cache even when the playing input does) and locate the same
  // stream index; container stream order is stable for a given input, so id
  // maps 1:1.
  AVFormatContext* ctx = nullptr;
  int ret = avformat_open_input(&ctx, uri.c_str(), nullptr, nullptr);
  if (ret < 0) {
    return fail(
        fmt::format("exportSubtitleText: cannot reopen '{}': {}", uri,
                    avError(ret)),
        /*emit_event=*/false);
  }

  std::string error;
  AVCodecContext* decoder = nullptr;
  AVPacket* packet = av_packet_alloc();
  const AVStream* stream =
      id < static_cast<TrackId>(ctx->nb_streams) ? ctx->streams[id] : nullptr;
  bool ready = stream && stream->codecpar &&
               stream->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE &&
               stream->codecpar->codec_id != AV_CODEC_ID_NONE;
  if (!ready && avformat_find_stream_info(ctx, nullptr) >= 0) {
    // The header usually already names the codec (MKV carries codec id and
    // private data in the segment header); this pass only probes when it
    // does not.
    stream = id < static_cast<TrackId>(ctx->nb_streams) ? ctx->streams[id]
                                                        : nullptr;
    ready = stream && stream->codecpar &&
            stream->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE &&
            stream->codecpar->codec_id != AV_CODEC_ID_NONE;
  }
  if (!ready) {
    error = "exportSubtitleText: no subtitle stream at that id in the "
            "reopened input";
  } else if (packet != nullptr) {
    decoder = buildSubtitleDecoder(ctx, id, "exportSubtitleText", error);
  } else {
    error = "exportSubtitleText: failed to allocate packet";
  }

  std::string srt;
  int cue_count = 0;
  if (decoder != nullptr) {
    while ((ret = av_read_frame(ctx, packet)) >= 0) {
      if (packet->stream_index != id) {
        av_packet_unref(packet);
        continue;
      }
      // Same decode interface as the decode loop (subtitle decoders do not
      // go through avcodec_send_packet/receive_frame) and the same timing
      // rules: display times are relative to the packet pts, and the ASS
      // decoder leaves end_display_time at 0 so the cue length falls back
      // to the packet duration.
      AVSubtitle sub;
      std::memset(&sub, 0, sizeof(sub));
      int got_sub = 0;
      ret = avcodec_decode_subtitle2(decoder, &sub, &got_sub, packet);
      if (ret < 0 || got_sub == 0) {
        av_packet_unref(packet);
        continue;
      }

      const AVStream* st = ctx->streams[id];
      const int64_t ts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
      auto pts = fromAVTimestamp(ts, st->time_base.num, st->time_base.den);
      auto duration = std::chrono::milliseconds(
          sub.end_display_time > sub.start_display_time
              ? sub.end_display_time - sub.start_display_time
              : 0);
      if (duration.count() <= 0 && packet->duration > 0 &&
          packet->duration != AV_NOPTS_VALUE) {
        duration = fromAVTimestamp(packet->duration, st->time_base.num,
                                   st->time_base.den);
      }
      if (duration.count() <= 0) {
        duration = kDefaultCueDuration;
      }

      // Text payloads only — processSubtitleFrame's render-state gates do
      // not apply here (nothing is being drawn). Bitmap rects contribute
      // nothing; a track that yields no text at all fails below.
      std::string text;
      for (unsigned i = 0; i < sub.num_rects; ++i) {
        const AVSubtitleRect* rect = sub.rects[i];
        if (rect == nullptr) {
          continue;
        }
        std::string piece;
        if (rect->type == SUBTITLE_TEXT && rect->text != nullptr) {
          piece = rect->text;
        } else if (rect->type == SUBTITLE_ASS && rect->ass != nullptr) {
          piece = assDialogueText(rect->ass);
        }
        if (piece.empty()) {
          continue;
        }
        if (!text.empty()) {
          text += '\n';
        }
        text += piece;
      }
      avsubtitle_free(&sub);
      if (text.empty()) {
        av_packet_unref(packet);
        continue;
      }

      ++cue_count;
      srt += std::to_string(cue_count);
      srt += '\n';
      srt += srtTimestamp(pts);
      srt += " --> ";
      srt += srtTimestamp(pts + duration);
      srt += '\n';
      srt += text;
      srt += "\n\n";
      av_packet_unref(packet);
    }
  }

  avcodec_free_context(&decoder);
  av_packet_free(&packet);
  avformat_close_input(&ctx);

  if (cue_count == 0) {
    return fail(error.empty()
                    ? "exportSubtitleText: no text cues in the track"
                    : error,
                /*emit_event=*/false);
  }
  out_srt = std::move(srt);
  return true;
}

//=============================================================================
// A-B loop
//=============================================================================

bool FFmpegBackend::setLoopAB(std::chrono::milliseconds a, std::chrono::milliseconds b) {
  {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    if (!format_ctx_) {
      return fail("setLoopAB: no media opened");
    }
  }
  const auto info = mediaInfo();
  if (!info.seekable) {
    return fail("setLoopAB: media is not seekable");
  }
  if (a < std::chrono::milliseconds(0) || a >= b || b > info.duration) {
    return fail("setLoopAB: loop window must satisfy 0 <= A < B <= duration");
  }
  loop_a_ms_.store(a.count(), std::memory_order_relaxed);
  loop_b_ms_.store(b.count(), std::memory_order_relaxed);
  return true;
}

bool FFmpegBackend::clearLoopAB() {
  {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    if (!format_ctx_) {
      return fail("clearLoopAB: no media opened");
    }
  }
  loop_a_ms_.store(-1, std::memory_order_relaxed);
  loop_b_ms_.store(-1, std::memory_order_relaxed);
  return true;
}

bool FFmpegBackend::loopAB(std::chrono::milliseconds& out_a, std::chrono::milliseconds& out_b) const {
  const auto a = loop_a_ms_.load(std::memory_order_relaxed);
  const auto b = loop_b_ms_.load(std::memory_order_relaxed);
  if (a < 0 || b < 0) {
    return false;
  }
  out_a = std::chrono::milliseconds(a);
  out_b = std::chrono::milliseconds(b);
  return true;
}

std::vector<std::string> FFmpegBackend::audioOutputDevices() const {
#ifndef SOAR_WITH_SDL2
  return {};
#else
  // Enumeration needs the audio subsystem but not an open stream; without
  // a usable driver (headless CI) SDL reports zero devices and the caller
  // sees an empty list.
  if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
      return {};
    }
  }
  std::vector<std::string> names;
  const int count = SDL_GetNumAudioDevices(/*iscapture=*/0);
  for (int i = 0; i < count; ++i) {
    const char* name = SDL_GetAudioDeviceName(i, /*iscapture=*/0);
    if (name && *name) {
      names.emplace_back(name);
    }
  }
  return names;
#endif
}

std::string FFmpegBackend::currentAudioOutputDevice() const {
  std::lock_guard<std::mutex> lock(audio_device_mutex_);
  return audio_device_;
}

bool FFmpegBackend::selectAudioOutputDevice(const std::string& name) {
  // A UI-level mistake, not a playback error: lastError() only (the
  // saveScreenshot precedent — no Error event, no state change).
  if (!name.empty()) {
    const std::vector<std::string> known = audioOutputDevices();
    if (std::find(known.begin(), known.end(), name) == known.end()) {
      return fail("selectAudioOutputDevice: unknown device '" + name + "'", /*emit_event=*/false);
    }
  }
  {
    std::lock_guard<std::mutex> lock(audio_device_mutex_);
    audio_device_ = name;
  }
  // Live playback: the decode thread's next ensureOpen() re-opens on the
  // new endpoint (the name is part of its reuse check), so the switch
  // lands within one audio frame. What is still queued on the old device
  // plays out first — that is the shortest, least audible switch, and it
  // costs the UI thread nothing. A switch made while paused applies on
  // resume, for the same reason.
  return true;
}

bool FFmpegBackend::checkLoopWrap(bool at_eof) {
  if (loop_b_ms_.load(std::memory_order_relaxed) < 0) {
    return false;
  }

  if (!at_eof) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (playback_state_ != PlaybackState::Playing) {
      return false;
    }
    const auto rate = playback_rate_.load();
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - clock_origin_).count());
    const auto position = std::chrono::milliseconds(
        static_cast<std::chrono::milliseconds::rep>(elapsed.count() * rate));
    if (position < std::chrono::milliseconds(loop_b_ms_.load(std::memory_order_relaxed))) {
      return false;
    }
  }

  // Route the wrap through the decode loop's own seek machinery (same
  // channel the public seek() uses while a decode thread runs): it rebases
  // the play clock, clears the frame queues and emits PositionChanged, so
  // the wrap is indistinguishable from a user seek back to A — except that
  // it must not disarm the loop it is serving.
  seek_target_.store(loop_a_ms_.load(std::memory_order_relaxed), std::memory_order_relaxed);
  seek_requested_.store(true, std::memory_order_relaxed);
  decode_cv_.notify_all();
  return true;
}

//=============================================================================
// FFmpeg Context Management
//=============================================================================

// Defined here (not in the header) so HttpCache and the FFmpeg buffer
// pointers stay implementation details. `pos` is the logical read offset
// the sequential read callback serves from; avio keeps it in sync via the
// seek callback.
struct FFmpegBackend::AvioCacheContext {
  std::unique_ptr<HttpCache> cache;
  AVIOContext* pb = nullptr;
  uint8_t* buffer = nullptr;  // owned by pb after avio_alloc_context
  uint64_t pos = 0;
  // The static avio callbacks need the backend for the progress emitter's
  // decode-loop gate and emit(); set at the one construction site, so it is
  // never null.
  FFmpegBackend* owner = nullptr;
  // Download-progress throttle state (P3c). Confined to whichever thread
  // currently owns cache reads — the decode loop while playing, the seek
  // caller otherwise — mirroring HttpCache's own one-reader-at-a-time
  // contract, so a plain field is enough.
  bool progress_calibrated = false;
  uint64_t last_progress_step = 0;
};

int FFmpegBackend::avioReadCallback(void* opaque, uint8_t* buf, int buf_size) {
  auto* state = static_cast<AvioCacheContext*>(opaque);
  if (!state || buf_size <= 0) {
    return AVERROR(EIO);
  }
  const size_t n = state->cache->read(state->pos, buf, static_cast<size_t>(buf_size));
  if (n == HttpCache::npos) {
    return AVERROR(EIO);  // state->cache->error() carries the reason
  }
  if (n == 0) {
    // End of source: avio's read callback must signal AVERROR_EOF.
    // Returning 0 as success is not a defined outcome — some FFmpeg
    // versions interpret it as "no data yet" and retry the callback
    // forever, spinning the decode thread at end-of-file instead of
    // ending playback (found by the P3c windowed cache test).
    return AVERROR_EOF;
  }
  state->pos += n;

  // P3c download progress: report while playback pulls new blocks through
  // the cache. Throttled to 1/16 steps of the source so the sink sees a
  // bounded monotonic series ending at downloaded == total; a fully-cached
  // (offline) session never changes the step and stays silent. The decode
  // loop gate keeps the emit off the open()/stopped-seek paths, which run
  // on the caller's thread while holding decode_mutex_.
  if (state->owner->decode_loop_running_.load(std::memory_order_relaxed)) {
    const uint64_t total = state->cache->size();
    const uint64_t have = state->cache->cachedBytes();
    const uint64_t step = total > 0 ? have * 16 / total : 0;
    if (!state->progress_calibrated) {
      // First read only calibrates: an offline session starts with
      // cachedBytes already high, and step 0 would otherwise fake one
      // bogus "progress" event.
      state->progress_calibrated = true;
      state->last_progress_step = step;
    } else if (step != state->last_progress_step) {
      state->last_progress_step = step;
      Event e;
      e.type = EventType::DownloadProgress;
      // Payload rides in message as "downloaded/total" — Event must not
      // grow fields (coverage-notes §3.8).
      e.message = fmt::format("{}/{}", have, total);
      state->owner->emit(e);
    }
  }
  return static_cast<int>(n);
}

int64_t FFmpegBackend::avioSeekCallback(void* opaque, int64_t offset, int whence) {
  auto* state = static_cast<AvioCacheContext*>(opaque);
  if (!state) {
    return AVERROR(EIO);
  }
  // Size probe: return the total length without moving (avio uses this to
  // size seeks-to-end and demuxer size queries).
  if ((whence & AVSEEK_SIZE) != 0) {
    return static_cast<int64_t>(state->cache->size());
  }
  int64_t target = 0;
  switch (whence) {
    case SEEK_SET:
      target = offset;
      break;
    case SEEK_CUR:
      target = static_cast<int64_t>(state->pos) + offset;
      break;
    case SEEK_END:
      target = static_cast<int64_t>(state->cache->size()) + offset;
      break;
    default:
      return AVERROR(EINVAL);
  }
  if (target < 0) {
    return AVERROR(EINVAL);
  }
  state->pos = static_cast<uint64_t>(target);
  return target;
}

int FFmpegBackend::ffmpegInterruptCallback(void* opaque) {
  auto* self = static_cast<FFmpegBackend*>(opaque);
  if (!self) {
    return 0;
  }
  if (self->should_stop_decoding_.load()) {
    return 1;
  }
  const auto started_ms = self->network_read_started_ms_.load();
  if (started_ms == 0) {
    // Not inside a watched media read (open/probe phase, paused, or between
    // reads): keep the pre-existing stop-only behavior.
    return 0;
  }
  const auto quiet = std::chrono::steady_clock::now().time_since_epoch() -
                     std::chrono::milliseconds(started_ms);
  if (quiet >= kNetworkStallLimitMs) {
    // Out of tolerance: abort this read. decodeLoop tells this apart from
    // a plain stop() via network_stall_exceeded_.
    self->network_stall_exceeded_.store(true);
    return 1;
  }
  if (quiet >= kNetworkStallReportMs) {
    // Quiet but still within tolerance: report buffering once per read and
    // keep waiting — FFmpeg keeps polling the same, still-open connection.
    if (!self->network_stall_reported_.exchange(true)) {
      self->emit(Event{EventType::BufferingStarted});
    }
  }
  return 0;
}

bool FFmpegBackend::openContext(const MediaSource& source, AVFormatContext** out_ctx) {
  // A leftover cache session (e.g. a previous open() that failed before
  // closeContext ever ran) must not outlive its format context.
  if (avio_cache_ && avio_cache_->pb) {
    avio_context_free(&avio_cache_->pb);
  }
  avio_cache_.reset();

  AVFormatContext* ctx = avformat_alloc_context();
  if (!ctx) {
    return fatal("open: failed to allocate format context", /*emit_event=*/false);
  }

  // The interrupt callback doubles as the network stall watchdog (see
  // ffmpegInterruptCallback): during media reads it reports a quiet source
  // and aborts only past the tolerance window, so a stalled network source
  // surfaces as buffering events instead of a silent hang.
  ctx->interrupt_callback.callback = &ffmpegInterruptCallback;
  ctx->interrupt_callback.opaque = this;

  // Disk cache for http:// sources (MediaSource::cache_dir). The HttpCache
  // constructor probes the source size with a Range request; only https://
  // stays on the direct path (no TLS dependency in this component).
  const bool use_cache =
    !source.cache_dir.empty() && source.uri.rfind("http://", 0) == 0;
  if (use_cache) {
    auto state = std::make_unique<AvioCacheContext>();
    state->owner = this;
    state->cache = std::make_unique<HttpCache>(source.cache_dir, source.uri);
    if (!state->cache->valid()) {
      const std::string err = state->cache->error();
      avformat_free_context(ctx);
      return fatal(
        fmt::format("open: cache setup failed for '{}': {}", source.uri, err),
        /*emit_event=*/false);
    }

    // A non-null seek callback makes avio_alloc_context set AVIO_SEEKABLE_NORMAL,
    // so MediaInfo::seekable and av_seek_frame work unchanged (verified against
    // FFmpeg 6.1; the custom-pb path only ever issues SEEK_SET/CUR and
    // AVSEEK_SIZE — SEEK_END is implemented defensively anyway).
    constexpr int kAvioBufferSize = 32 * 1024;
    state->buffer = static_cast<uint8_t*>(av_malloc(kAvioBufferSize));
    if (!state->buffer) {
      avformat_free_context(ctx);
      return fatal("open: failed to allocate AVIO cache buffer", /*emit_event=*/false);
    }
    state->pb = avio_alloc_context(
      state->buffer, kAvioBufferSize,
      /*write_flag=*/0, state.get(),
      &avioReadCallback, /*write_packet=*/nullptr, &avioSeekCallback);
    if (!state->pb) {
      av_free(state->buffer);
      avformat_free_context(ctx);
      return fatal("open: failed to allocate AVIO cache context", /*emit_event=*/false);
    }

    ctx->pb = state->pb;
    // Custom IO: pb is ours; avformat_close_input must not free it
    // (closeContext does, and avio_context_free also releases buffer).
    ctx->flags |= AVFMT_FLAG_CUSTOM_IO;
    avio_cache_ = std::move(state);
  }

  int ret = avformat_open_input(&ctx, source.uri.c_str(), nullptr, nullptr);
  if (ret < 0) {
    // avformat_open_input frees ctx itself on failure (and leaves a custom
    // pb alone), so only our AVIO state needs tearing down here.
    if (avio_cache_) {
      if (avio_cache_->pb) {
        avio_context_free(&avio_cache_->pb);
      }
      avio_cache_.reset();
    }
    return fatal(fmt::format("open: failed to open '{}': {}", source.uri, avError(ret)), /*emit_event=*/false);
  }

  *out_ctx = ctx;
  return true;
}

void FFmpegBackend::closeContext() {
  if (format_ctx_) {
    avformat_close_input(&format_ctx_);
    format_ctx_ = nullptr;
  }

  // With AVFMT_FLAG_CUSTOM_IO the AVIOContext is ours: avformat_close_input
  // leaves it alone. avio_context_free also frees the buffer passed at
  // allocation — verified against FFmpeg 6.1 that av_free()ing it again is
  // a double free, so only the context is freed here.
  if (avio_cache_ && avio_cache_->pb) {
    avio_context_free(&avio_cache_->pb);
  }
  avio_cache_.reset();

  video_stream_index_ = -1;
  audio_stream_index_ = -1;
  subtitle_stream_index_ = -1;
}

bool FFmpegBackend::findStreamInfo() {
  int ret = avformat_find_stream_info(format_ctx_, nullptr);
  if (ret < 0) {
    return fatal(fmt::format("findStreamInfo: failed: {}", avError(ret)), /*emit_event=*/false);
  }

  video_stream_index_ = pickBestStreamIndex(format_ctx_, AVMEDIA_TYPE_VIDEO);
  audio_stream_index_ = pickBestStreamIndex(format_ctx_, AVMEDIA_TYPE_AUDIO);
  subtitle_stream_index_ = pickBestStreamIndex(format_ctx_, AVMEDIA_TYPE_SUBTITLE);

  if (video_stream_index_ < 0 && audio_stream_index_ < 0) {
    return fatal("findStreamInfo: no video or audio stream found", /*emit_event=*/false);
  }

  return true;
}

bool FFmpegBackend::setupDecoders() {
  const AVCodec* codec = nullptr;

  // Setup video decoder
  if (video_stream_index_ >= 0) {
    AVStream* stream = format_ctx_->streams[video_stream_index_];
    AVCodecParameters* codecpar = stream->codecpar;

    codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
      return fatal(fmt::format("setupDecoders: video codec not found: {}", static_cast<int>(codecpar->codec_id)), /*emit_event=*/false);
    }

    video_decoder_ = avcodec_alloc_context3(codec);
    if (!video_decoder_) {
      return fatal("setupDecoders: failed to allocate video decoder context", /*emit_event=*/false);
    }

    int ret = avcodec_parameters_to_context(video_decoder_, codecpar);
    if (ret < 0) {
      return fatal(fmt::format("setupDecoders: failed to copy video params: {}", avError(ret)), /*emit_event=*/false);
    }

    ret = avcodec_open2(video_decoder_, codec, nullptr);
    if (ret < 0) {
      return fatal(fmt::format("setupDecoders: failed to open video decoder: {}", avError(ret)), /*emit_event=*/false);
    }

    // Store video parameters
    video_params_.width = video_decoder_->width;
    video_params_.height = video_decoder_->height;
    video_params_.pix_fmt = video_decoder_->pix_fmt;
  }

  // Setup audio decoder
  if (audio_stream_index_ >= 0) {
    AVStream* stream = format_ctx_->streams[audio_stream_index_];
    AVCodecParameters* codecpar = stream->codecpar;

    codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
      return fatal(fmt::format("setupDecoders: audio codec not found: {}", static_cast<int>(codecpar->codec_id)), /*emit_event=*/false);
    }

    audio_decoder_ = avcodec_alloc_context3(codec);
    if (!audio_decoder_) {
      return fatal("setupDecoders: failed to allocate audio decoder context", /*emit_event=*/false);
    }

    int ret = avcodec_parameters_to_context(audio_decoder_, codecpar);
    if (ret < 0) {
      return fatal(fmt::format("setupDecoders: failed to copy audio params: {}", avError(ret)), /*emit_event=*/false);
    }

    ret = avcodec_open2(audio_decoder_, codec, nullptr);
    if (ret < 0) {
      return fatal(fmt::format("setupDecoders: failed to open audio decoder: {}", avError(ret)), /*emit_event=*/false);
    }

    // Store audio parameters (FFmpeg 8.0+ uses ch_layout)
    audio_params_.sample_rate = audio_decoder_->sample_rate;
    audio_params_.channels = audio_decoder_->ch_layout.nb_channels;
    // Store channel layout as uint64_t for compatibility
    audio_params_.channel_layout = audio_decoder_->ch_layout.u.mask;
  }

  // Setup subtitle decoder
  if (subtitle_stream_index_ >= 0) {
    AVStream* stream = format_ctx_->streams[subtitle_stream_index_];
    AVCodecParameters* codecpar = stream->codecpar;

    codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
      return fatal(fmt::format("setupDecoders: subtitle codec not found: {}", static_cast<int>(codecpar->codec_id)), /*emit_event=*/false);
    }

    subtitle_decoder_ = avcodec_alloc_context3(codec);
    if (!subtitle_decoder_) {
      return fatal("setupDecoders: failed to allocate subtitle decoder context", /*emit_event=*/false);
    }

    int ret = avcodec_parameters_to_context(subtitle_decoder_, codecpar);
    if (ret < 0) {
      return fatal(fmt::format("setupDecoders: failed to copy subtitle params: {}", avError(ret)), /*emit_event=*/false);
    }

    ret = avcodec_open2(subtitle_decoder_, codec, nullptr);
    if (ret < 0) {
      return fatal(fmt::format("setupDecoders: failed to open subtitle decoder: {}", avError(ret)), /*emit_event=*/false);
    }

    // The default subtitle stream decodes from open: selection semantics
    // (selectTrack/disableSubtitles) only gate it afterwards, never here —
    // the plain-text queue is the open-state surface, so "subtitles are
    // on" stays true before any selection is made.
    subtitle_decode_active_ = true;

    // Style-faithful ASS (docs/mvp.md §6): an embedded ASS/SSA track feeds
    // libass — Matroska font attachments register first (so the script's
    // font lookups resolve from the file's own embedded fonts), then a
    // fresh streaming track takes the CodecPrivate (script header +
    // styles). Dialogue lines arrive per-packet in processSubtitleFrame.
    // Without libass (stub) or for text codecs nothing here activates and
    // the plain-text path stays authoritative.
    ass_feed_active_ = false;
    ass_document_active_ = false;
    if (ass_renderer_.available()) {
      // Attachments register for the whole open, not just the embedded
      // feed: addFont fills the library, which survives track swaps, and
      // an external .ass document selected later may name the same
      // families its file embeds.
      for (unsigned i = 0; i < format_ctx_->nb_streams; ++i) {
        const AVStream* st = format_ctx_->streams[i];
        const AVCodecParameters* par = st ? st->codecpar : nullptr;
        if (!par || par->codec_type != AVMEDIA_TYPE_ATTACHMENT ||
            (par->codec_id != AV_CODEC_ID_TTF && par->codec_id != AV_CODEC_ID_OTF) ||
            par->extradata_size <= 0 || par->extradata == nullptr) {
          continue;
        }
        const AVDictionaryEntry* font_name =
            st->metadata
                ? av_dict_get(st->metadata, "filename", nullptr, 0)
                : nullptr;
        ass_renderer_.addFont(
            font_name && font_name->value ? font_name->value : "font",
            reinterpret_cast<const std::uint8_t*>(par->extradata),
            static_cast<std::size_t>(par->extradata_size));
      }
      if (codecpar->codec_id == AV_CODEC_ID_ASS ||
          codecpar->codec_id == AV_CODEC_ID_SSA) {
        if (ass_renderer_.startStream()) {
          if (codecpar->extradata_size > 0 && codecpar->extradata != nullptr) {
            ass_renderer_.feedCodecPrivate(
                reinterpret_cast<const char*>(codecpar->extradata),
                static_cast<std::size_t>(codecpar->extradata_size));
          }
          if (video_stream_index_ >= 0) {
            const AVCodecParameters* vpar =
                format_ctx_->streams[video_stream_index_]->codecpar;
            if (vpar->width > 0 && vpar->height > 0) {
              ass_renderer_.setFrameSize(vpar->width, vpar->height);
            }
          }
          ass_feed_active_ = true;
        }
      }
    }
  }

  return true;
}

void FFmpegBackend::cleanupDecoders() {
  // Cleanup video decoder
  if (video_decoder_) {
    avcodec_free_context(&video_decoder_);
    video_decoder_ = nullptr;
  }

  // Cleanup audio decoder
  if (audio_decoder_) {
    avcodec_free_context(&audio_decoder_);
    audio_decoder_ = nullptr;
  }

  // Cleanup subtitle decoder
  if (subtitle_decoder_) {
    avcodec_free_context(&subtitle_decoder_);
    subtitle_decoder_ = nullptr;
  }
  // No more feeding once the decoders are gone (the renderer keeps its
  // last track; the next open re-runs startStream in setupDecoders). The
  // document flag and decode gate belong to the closed media too —
  // both are rebuilt by the next open.
  ass_feed_active_ = false;
  ass_document_active_ = false;
  subtitle_decode_active_ = false;

  // Cleanup resamplers
  if (audio_resampler_) {
    swr_free(&audio_resampler_);
    audio_resampler_ = nullptr;
  }

  if (video_scaler_) {
    sws_freeContext(video_scaler_);
    video_scaler_ = nullptr;
  }

  video_convert_.reset();
}

//=============================================================================
// Decoding Loop
//=============================================================================

void FFmpegBackend::decodeLoop() {
  AVPacket* packet = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();

  if (!packet || !frame) {
    fatal("decodeLoop: failed to allocate packet/frame");
    return;
  }

  bool fatal_decode_error = false;
  // Start each session with clean network-watchdog state (a previous
  // session may have aborted mid-read).
  network_read_started_ms_.store(0);
  network_stall_reported_.store(false);
  network_stall_exceeded_.store(false);
  // Arms the download-progress emitter: from here until the loop exits,
  // cache reads are playback reads, not open()-time header probes.
  decode_loop_running_.store(true, std::memory_order_relaxed);
  while (!should_stop_decoding_) {
    // Check for pause/stop
    {
      std::unique_lock<std::mutex> lock(state_mutex_);
      decode_cv_.wait(lock, [this] {
        return should_stop_decoding_ ||
               playback_state_ == PlaybackState::Playing ||
               seek_requested_.load() ||
               audioTrackPending() ||
               subtitleTrackPending();
      });
    }

    if (should_stop_decoding_) {
      break;
    }

    // Handle a pending audio track switch first, so a queued seek keeps
    // the final say about the resume position.
    if (audioTrackPending()) {
      applyPendingAudioTrack();
      if (should_stop_decoding_) {
        break;
      }
      continue;
    }

    // Then a pending subtitle switch, ahead of a seek for the same
    // reason: the seek that follows rescans cues onto the new stream.
    if (subtitleTrackPending()) {
      applyPendingSubtitleTrack();
      if (should_stop_decoding_) {
        break;
      }
      continue;
    }

    // Handle seek request
    if (seek_requested_) {
      auto target = std::chrono::milliseconds(seek_target_.load());
      seek_requested_ = false;

      if (!seekToTimestamp(target)) {
        continue;
      }

      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        current_position_ = target;
        last_emitted_position_ = target;
        const auto rate = playback_rate_.load();
        const auto now = std::chrono::steady_clock::now();
        const auto scaled = std::chrono::duration<double, std::milli>(target.count() / rate);
        clock_origin_ = now - std::chrono::duration_cast<std::chrono::steady_clock::duration>(scaled);
      }

      emit(Event{EventType::PositionChanged});

      // After a seek, drop any queued frames.
      {
        std::scoped_lock lock(video_queue_mutex_, audio_queue_mutex_);
        clearDecodedQueue(video_queue_);
        clearDecodedQueue(audio_queue_);
      }
      {
        std::lock_guard<std::mutex> lock(subtitle_frame_mutex_);
        subtitle_frames_ = {};
      }
    }

    // A-B loop: once the play clock reaches the armed B point, route a
    // seek back to A through the handler above. Checked per packet, so
    // the wrap overshoot is bounded by one frame's pacing latency.
    if (checkLoopWrap(/*at_eof=*/false)) {
      continue;
    }

    // Read packet. Timestamp the read so the interrupt callback — called
    // from FFmpeg's network poll loop while this blocks — can watch for a
    // stalled source (report buffering, abort past the tolerance window).
    network_read_started_ms_.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    int ret = av_read_frame(format_ctx_, packet);
    network_read_started_ms_.store(0);
    if (ret == 0 && network_stall_reported_.exchange(false)) {
      // Data resumed after at least one reported quiet window.
      emit(Event{EventType::BufferingEnded});
    }
    if (ret < 0) {
      if (ret == AVERROR_EXIT) {
        // stop() or the stall watchdog aborted the read; the exceeded flag
        // tells them apart.
        if (network_stall_exceeded_.exchange(false)) {
          fatal("decodeLoop: network source stalled beyond tolerance");
        }
        break;
      }
      if (ret == AVERROR_EOF) {
        // End of file — unless an A-B loop is armed: an end-anchored loop
        // (B == duration) replays from A instead of ending here.
        if (checkLoopWrap(/*at_eof=*/true)) {
          continue;
        }
        {
          std::lock_guard<std::mutex> lock(state_mutex_);
          playback_state_ = PlaybackState::Ended;
        }
        emit(Event{EventType::StateChanged});
        break;
      }
      // Error
      fatal(fmt::format("decodeLoop: av_read_frame failed: {}", avError(ret)));
      break;
    }

    // Decode based on stream type
    if (packet->stream_index == video_stream_index_) {
      // Send packet to video decoder
      ret = avcodec_send_packet(video_decoder_, packet);
      if (ret < 0) {
        av_packet_unref(packet);
        continue;
      }

      // Receive frames from video decoder
      while (ret >= 0) {
        ret = avcodec_receive_frame(video_decoder_, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
          break;
        }
        if (ret < 0) {
          fatal(fmt::format("decodeLoop: video decode failed: {}", avError(ret)));
          fatal_decode_error = true;
          break;
        }

        // Process video frame
        const int64_t ts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
        auto pts = fromAVTimestamp(
          ts,
          format_ctx_->streams[video_stream_index_]->time_base.num,
          format_ctx_->streams[video_stream_index_]->time_base.den
        );

        queueVideoFrame(frame, pts);
        av_frame_unref(frame);
      }
    } else if (packet->stream_index == audio_stream_index_) {
      // Send packet to audio decoder
      ret = avcodec_send_packet(audio_decoder_, packet);
      if (ret < 0) {
        av_packet_unref(packet);
        continue;
      }

      // Receive frames from audio decoder
      while (ret >= 0) {
        ret = avcodec_receive_frame(audio_decoder_, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
          break;
        }
        if (ret < 0) {
          fatal(fmt::format("decodeLoop: audio decode failed: {}", avError(ret)));
          fatal_decode_error = true;
          break;
        }

        // Process audio frame
        const int64_t ts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
        auto pts = fromAVTimestamp(
          ts,
          format_ctx_->streams[audio_stream_index_]->time_base.num,
          format_ctx_->streams[audio_stream_index_]->time_base.den
        );

        queueAudioFrame(frame, pts);
        av_frame_unref(frame);
      }
    } else if (packet->stream_index == subtitle_stream_index_ &&
               subtitle_decode_active_.load(std::memory_order_relaxed)) {
      // The gate is the selection (selectTrack/disableSubtitles): with a
      // sidecar selected or subtitles off, the embedded stream's packets
      // pass by undecoded — the overlay that batch 1b/1c could only close
      // on the canvas side closes at the source in every build.
      //
      // Subtitle decoders do not go through avcodec_send_packet/
      // avcodec_receive_frame: the generic decode path asserts on non-A/V
      // codec types (FFmpeg 8 decode.c av_assert0(0), found by the SRT
      // fixture — playback aborted mid-stream after the first events).
      // avcodec_decode_subtitle2 is the supported interface; it fills an
      // AVSubtitle whose display times are milliseconds relative to the
      // packet pts.
      AVSubtitle sub;
      std::memset(&sub, 0, sizeof(sub));
      int got_sub = 0;
      ret = avcodec_decode_subtitle2(subtitle_decoder_, &sub, &got_sub, packet);
      if (ret < 0) {
        av_packet_unref(packet);
        continue;
      }

      if (got_sub != 0) {
        const AVStream* stream = format_ctx_->streams[subtitle_stream_index_];
        const int64_t ts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
        auto pts = fromAVTimestamp(
          ts,
          stream->time_base.num,
          stream->time_base.den
        );
        auto duration = std::chrono::milliseconds(
          sub.end_display_time > sub.start_display_time
              ? sub.end_display_time - sub.start_display_time
              : 0
        );
        // The ASS decoder leaves end_display_time at 0 — the cue length
        // rides on the packet duration (Matroska BlockDuration). Without
        // this fallback every embedded ASS event would be zero-length and
        // never display (libass hides an event whose end <= start).
        if (duration.count() <= 0 && packet->duration > 0 &&
            packet->duration != AV_NOPTS_VALUE) {
          duration = fromAVTimestamp(packet->duration,
                                     stream->time_base.num,
                                     stream->time_base.den);
        }
        processSubtitleFrame(sub, pts, duration);
      }
      avsubtitle_free(&sub);
    }

    av_packet_unref(packet);
    drainFrameQueues();

    if (fatal_decode_error) {
      break;
    }
  }

  decode_loop_running_.store(false, std::memory_order_relaxed);
  av_frame_free(&frame);
  av_packet_free(&packet);
}

void FFmpegBackend::queueVideoFrame(AVFrame* frame, std::chrono::milliseconds pts) {
  AVFrame* stored = av_frame_alloc();
  if (!stored) {
    return;
  }

  av_frame_move_ref(stored, frame);
  DecodedFrame decoded{stored, pts};

  std::lock_guard<std::mutex> lock(video_queue_mutex_);
  video_queue_.push(decoded);
  while (video_queue_.size() > kMaxVideoQueueFrames) {
    auto dropped = video_queue_.front();
    video_queue_.pop();
    freeFrame(dropped.frame);
  }
}

void FFmpegBackend::queueAudioFrame(AVFrame* frame, std::chrono::milliseconds pts) {
  AVFrame* stored = av_frame_alloc();
  if (!stored) {
    return;
  }

  av_frame_move_ref(stored, frame);
  DecodedFrame decoded{stored, pts};

  std::lock_guard<std::mutex> lock(audio_queue_mutex_);
  audio_queue_.push(decoded);
  while (audio_queue_.size() > kMaxAudioQueueFrames) {
    auto dropped = audio_queue_.front();
    audio_queue_.pop();
    freeFrame(dropped.frame);
  }
}

void FFmpegBackend::queueSubtitleFrame(const std::string& text, std::chrono::milliseconds pts, std::chrono::milliseconds duration) {
  if (text.empty()) {
    return;
  }
  DecodedSubtitleFrame sub;
  sub.text = text;
  sub.pts = pts;
  sub.duration = duration;

  std::lock_guard<std::mutex> lock(subtitle_frame_mutex_);
  subtitle_frames_.push(std::move(sub));
  while (subtitle_frames_.size() > kMaxSubtitleQueueFrames) {
    subtitle_frames_.pop();
  }
}

void FFmpegBackend::drainFrameQueues() {
  while (!should_stop_decoding_) {
    DecodedFrame next{};
    bool is_video = false;
    {
      std::scoped_lock lock(video_queue_mutex_, audio_queue_mutex_);
      if (video_queue_.empty() && audio_queue_.empty()) {
        break;
      }

      if (!video_queue_.empty() && !audio_queue_.empty()) {
        const auto& v = video_queue_.front();
        const auto& a = audio_queue_.front();
        is_video = v.pts <= a.pts;
      } else if (!video_queue_.empty()) {
        is_video = true;
      } else {
        is_video = false;
      }

      if (is_video) {
        next = video_queue_.front();
        video_queue_.pop();
      } else {
        next = audio_queue_.front();
        audio_queue_.pop();
      }
    }

    if (!next.frame) {
      continue;
    }

    if (is_video) {
      processVideoFrame(next);
    } else {
      processAudioFrame(next);
    }
  }
}

bool FFmpegBackend::waitForPresentationTime(std::chrono::milliseconds pts) {
  while (!should_stop_decoding_) {
    std::unique_lock<std::mutex> lock(state_mutex_);

    if (seek_requested_) {
      return false;
    }

    if (playback_state_ != PlaybackState::Playing) {
      decode_cv_.wait(lock, [this] {
        return should_stop_decoding_ ||
               seek_requested_.load() ||
               playback_state_ == PlaybackState::Playing;
      });
      continue;
    }

    const auto rate = playback_rate_.load();
    const auto scaled = std::chrono::duration<double, std::milli>(pts.count() / rate);
    const auto due = clock_origin_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(scaled);

    if (std::chrono::steady_clock::now() >= due) {
      return true;
    }

    decode_cv_.wait_until(lock, due, [this] {
      return should_stop_decoding_ || seek_requested_.load() || playback_state_ != PlaybackState::Playing;
    });
  }

  return false;
}

void FFmpegBackend::processVideoFrame(DecodedFrame frame) {
  const auto pts = frame.pts;
  if (!waitForPresentationTime(pts)) {
    freeFrame(frame.frame);
    return;
  }

  bool should_emit = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    current_position_ = pts;
    if ((current_position_ - last_emitted_position_) >= kPositionEmitGranularity ||
        (last_emitted_position_ - current_position_) >= kPositionEmitGranularity) {
      last_emitted_position_ = current_position_;
      should_emit = true;
    }
  }

  if (should_emit) {
    emit(Event{EventType::PositionChanged});
  }

  renderVideoFrame(frame.frame);
  freeFrame(frame.frame);
}

void FFmpegBackend::processAudioFrame(DecodedFrame frame) {
  const auto pts = frame.pts;
  if (!waitForPresentationTime(pts)) {
    freeFrame(frame.frame);
    return;
  }

  const bool has_video = video_stream_index_ >= 0;
  bool should_emit = false;
  if (!has_video) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    current_position_ = pts;
    if ((current_position_ - last_emitted_position_) >= kPositionEmitGranularity ||
        (last_emitted_position_ - current_position_) >= kPositionEmitGranularity) {
      last_emitted_position_ = current_position_;
      should_emit = true;
    }
  }

  if (should_emit) {
    emit(Event{EventType::PositionChanged});
  }

  playAudioFrame(frame.frame);
  freeFrame(frame.frame);
}

AssRenderer* FFmpegBackend::assRenderer() {
  // Stable for the backend's lifetime; AssRenderer locks internally, so
  // the UI may render while the decode thread feeds.
  return ass_renderer_.available() ? &ass_renderer_ : nullptr;
}

void FFmpegBackend::processSubtitleFrame(const AVSubtitle& sub, std::chrono::milliseconds pts, std::chrono::milliseconds duration) {
  // Subtitle frames don't wait for presentation time; they're queued
  // immediately and the UI renders them based on current position.
  // We still respect should_stop_decoding_ to avoid queuing after shutdown.
  if (should_stop_decoding_.load()) {
    return;
  }

  // Pull the human-readable payload out of the rects. The SRT/WebVTT
  // decoders emit ASS-style rects; plain TEXT rects come from a few
  // legacy decoders. An empty rect list is a "clear" event, which
  // queueSubtitleFrame drops (timed subs expire by their own duration in
  // the UI instead).
  std::string text;
  for (unsigned i = 0; i < sub.num_rects; ++i) {
    const AVSubtitleRect* rect = sub.rects[i];
    if (rect == nullptr) {
      continue;
    }
    std::string piece;
    if (rect->type == SUBTITLE_TEXT && rect->text != nullptr) {
      if (ass_document_active_.load(std::memory_order_relaxed)) {
        // The same drop-gate as the ASS rects below: a document owns the
        // canvas, and a plain-text echo of this stream under it would
        // draw every line twice.
        continue;
      }
      // Plain rects join the frame's text; whether that text feeds the
      // canvas (one synthesized Dialogue event per frame, below) or
      // queues for the UI path is a per-frame decision, not a per-rect
      // one — a decoder may hand back one rect per line.
      piece = rect->text;
    } else if (rect->type == SUBTITLE_ASS && rect->ass != nullptr) {
      if (ass_document_active_.load(std::memory_order_relaxed)) {
        // The renderer is showing an external document: the embedded
        // stream's events are deselected, not degraded — drop them, or
        // the document gets a plain-text echo of the stream drawn on
        // top of it.
        continue;
      }
      if (ass_feed_active_.load(std::memory_order_relaxed)) {
        // Style-faithful path: the real ASS decoder hands back complete
        // "Dialogue:" lines — feed verbatim and skip the plain-text
        // queue so the subtitle is not double-rendered (the UI draws
        // the libass canvas instead of the text overlay).
        ass_renderer_.feedEvent(
            assDialogueLine(rect->ass, pts, duration).c_str());
        continue;
      }
      piece = assDialogueText(rect->ass);
    }
    if (piece.empty()) {
      continue;
    }
    if (!text.empty()) {
      text += '\n';
    }
    text += piece;
  }

  if (!text.empty()) {
    if (ass_feed_active_.load(std::memory_order_relaxed)) {
      // A selected text stream renders on the canvas (embedded text
      // tracks joining batch 1c's contract): the frame's joined lines
      // become one Dialogue event against the synthesized default
      // header. The plain-text queue is skipped so the subtitle is not
      // double-rendered — ASS rects in feed mode already fed themselves
      // in the loop above, so this arm only ever sees text-rect frames.
      ass_renderer_.feedEvent(
          assDialogueLineFromText(text, pts, pts + duration).c_str());
      return;
    }
    queueSubtitleFrame(text, pts, duration);
  }
}

void FFmpegBackend::renderVideoFrame(const AVFrame* frame) {
  if (!frame) {
    return;
  }

  const int width = frame->width;
  const int height = frame->height;
  if (width <= 0 || height <= 0) {
    return;
  }

  const auto src_fmt = static_cast<AVPixelFormat>(frame->format);

  const AVFrame* src = frame;
  if (src_fmt != AV_PIX_FMT_YUV420P) {
    if (!video_convert_) {
      video_convert_ = std::make_unique<VideoConvertState>();
    }

    const bool needs_reinit =
      video_convert_->width != width ||
      video_convert_->height != height ||
      video_convert_->src_fmt != src_fmt ||
      video_convert_->dst == nullptr;

    if (needs_reinit) {
      if (video_convert_->dst) {
        av_frame_free(&video_convert_->dst);
      }

      video_convert_->dst = av_frame_alloc();
      if (!video_convert_->dst) {
        return;
      }

      video_convert_->dst->format = AV_PIX_FMT_YUV420P;
      video_convert_->dst->width = width;
      video_convert_->dst->height = height;

      const int required = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, width, height, 1);
      if (required <= 0) {
        return;
      }

      video_convert_->buffer.assign(static_cast<std::size_t>(required), 0);
      int ret = av_image_fill_arrays(
        video_convert_->dst->data,
        video_convert_->dst->linesize,
        video_convert_->buffer.data(),
        AV_PIX_FMT_YUV420P,
        width,
        height,
        1
      );
      if (ret < 0) {
        return;
      }

      video_scaler_ = sws_getCachedContext(
        video_scaler_,
        width,
        height,
        src_fmt,
        width,
        height,
        AV_PIX_FMT_YUV420P,
        SWS_BILINEAR,
        nullptr,
        nullptr,
        nullptr
      );
      if (!video_scaler_) {
        return;
      }

      video_convert_->width = width;
      video_convert_->height = height;
      video_convert_->src_fmt = src_fmt;
    }

    sws_scale(
      video_scaler_,
      src->data,
      src->linesize,
      0,
      height,
      video_convert_->dst->data,
      video_convert_->dst->linesize
    );

    src = video_convert_->dst;
  }

  staging_video_frame_.width = width;
  staging_video_frame_.height = height;
  staging_video_frame_.stride_y = width;
  staging_video_frame_.stride_u = width / 2;
  staging_video_frame_.stride_v = width / 2;
  staging_video_frame_.pts = fromAVTimestamp(
    frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts,
    format_ctx_->streams[video_stream_index_]->time_base.num,
    format_ctx_->streams[video_stream_index_]->time_base.den
  );

  staging_video_frame_.y.resize(static_cast<std::size_t>(staging_video_frame_.stride_y * staging_video_frame_.height));
  staging_video_frame_.u.resize(
    static_cast<std::size_t>(staging_video_frame_.stride_u * (staging_video_frame_.height / 2))
  );
  staging_video_frame_.v.resize(
    static_cast<std::size_t>(staging_video_frame_.stride_v * (staging_video_frame_.height / 2))
  );

  for (int row = 0; row < staging_video_frame_.height; ++row) {
    std::memcpy(
      staging_video_frame_.y.data() + static_cast<std::size_t>(row * staging_video_frame_.stride_y),
      src->data[0] + static_cast<std::size_t>(row * src->linesize[0]),
      static_cast<std::size_t>(staging_video_frame_.stride_y)
    );
  }

  const int chroma_h = staging_video_frame_.height / 2;
  for (int row = 0; row < chroma_h; ++row) {
    std::memcpy(
      staging_video_frame_.u.data() + static_cast<std::size_t>(row * staging_video_frame_.stride_u),
      src->data[1] + static_cast<std::size_t>(row * src->linesize[1]),
      static_cast<std::size_t>(staging_video_frame_.stride_u)
    );
    std::memcpy(
      staging_video_frame_.v.data() + static_cast<std::size_t>(row * staging_video_frame_.stride_v),
      src->data[2] + static_cast<std::size_t>(row * src->linesize[2]),
      static_cast<std::size_t>(staging_video_frame_.stride_v)
    );
  }

  {
    std::lock_guard<std::mutex> lock(video_frame_mutex_);
    std::swap(latest_video_frame_, staging_video_frame_);
    video_frame_ready_ = true;
    // Retained copy for saveScreenshot(); the mailbox above is handed to
    // the app by swap, so nothing reliable survives there.
    presented_video_frame_ = latest_video_frame_;
  }
}

void FFmpegBackend::playAudioFrame(const AVFrame* frame) {
#ifndef SOAR_WITH_SDL2
  (void)frame;
  return;
#else
  if (!frame) {
    return;
  }

  const int in_rate = frame->sample_rate > 0 ? frame->sample_rate : audio_params_.sample_rate;
  const int in_channels = frame->ch_layout.nb_channels > 0 ? frame->ch_layout.nb_channels : audio_params_.channels;
  if (in_rate <= 0 || in_channels <= 0) {
    return;
  }

  if (!sdl_audio_) {
    sdl_audio_ = std::make_unique<SDLAudio>();
  }

  const double rate = std::clamp(playback_rate_.load(), 0.25, 4.0);
  const int target_rate = std::clamp(static_cast<int>(std::lround(in_rate * rate)), 8000, 192000);

  std::string sdl_err;
  std::string device_name;
  {
    std::lock_guard<std::mutex> lock(audio_device_mutex_);
    device_name = audio_device_;
  }
  if (!sdl_audio_->ensureOpen(target_rate, in_channels, device_name, sdl_err)) {
    return;
  }

  const AVSampleFormat out_fmt = AV_SAMPLE_FMT_FLT;
  const int out_rate = sdl_audio_->sample_rate;
  const int out_channels = sdl_audio_->channels;

  const AVSampleFormat in_fmt = static_cast<AVSampleFormat>(frame->format);

  std::uint64_t key = static_cast<std::uint64_t>(in_rate);
  key = (key * 1315423911u) ^ static_cast<std::uint64_t>(in_channels);
  key = (key * 1315423911u) ^ static_cast<std::uint64_t>(in_fmt);
  key = (key * 1315423911u) ^ static_cast<std::uint64_t>(out_rate);
  key = (key * 1315423911u) ^ static_cast<std::uint64_t>(out_channels);

  if (audio_resampler_ && audio_resample_key_ != key) {
    swr_free(&audio_resampler_);
    audio_resampler_ = nullptr;
    audio_resample_key_ = 0;
  }

  if (!audio_resampler_) {
    AVChannelLayout out_layout{};
    av_channel_layout_default(&out_layout, out_channels);

    AVChannelLayout in_layout{};
    if (frame->ch_layout.nb_channels > 0 && av_channel_layout_copy(&in_layout, &frame->ch_layout) >= 0) {
      // ok
    } else {
      av_channel_layout_default(&in_layout, in_channels);
    }

    SwrContext* swr = nullptr;
    if (swr_alloc_set_opts2(
          &swr,
          &out_layout,
          out_fmt,
          out_rate,
          &in_layout,
          in_fmt,
          in_rate,
          0,
          nullptr
        ) < 0) {
      av_channel_layout_uninit(&in_layout);
      av_channel_layout_uninit(&out_layout);
      return;
    }
    av_channel_layout_uninit(&in_layout);
    av_channel_layout_uninit(&out_layout);

    if (swr_init(swr) < 0) {
      swr_free(&swr);
      return;
    }
    audio_resampler_ = swr;
    audio_resample_key_ = key;
  }

  const int64_t delay = swr_get_delay(audio_resampler_, in_rate);
  const int out_samples = static_cast<int>(
    av_rescale_rnd(delay + frame->nb_samples, out_rate, in_rate, AV_ROUND_UP)
  );
  if (out_samples <= 0) {
    return;
  }

  std::vector<float> out_data(static_cast<std::size_t>(out_samples * out_channels));
  uint8_t* out_planes[1] = { reinterpret_cast<uint8_t*>(out_data.data()) };
  const uint8_t** in_planes = const_cast<const uint8_t**>(frame->extended_data);

  const int converted = swr_convert(audio_resampler_, out_planes, out_samples, in_planes, frame->nb_samples);
  if (converted <= 0) {
    return;
  }

  const std::size_t frames = static_cast<std::size_t>(converted);
  const double volume = muted_.load() ? 0.0 : std::clamp(volume_.load(), 0.0, 1.0);
  if (volume != 1.0) {
    for (std::size_t i = 0; i < frames * static_cast<std::size_t>(out_channels); ++i) {
      out_data[i] = static_cast<float>(out_data[i] * volume);
    }
  }

  // Backpressure: cap queued audio to ~500ms.
  const std::size_t bytes_per_second =
    static_cast<std::size_t>(out_rate) * static_cast<std::size_t>(out_channels) * sizeof(float);
  const std::size_t max_queued = bytes_per_second / 2;
  while (sdl_audio_->queuedBytes() > max_queued) {
    SDL_Delay(5);
  }

  const std::size_t out_bytes = frames * static_cast<std::size_t>(out_channels) * sizeof(float);
  (void)sdl_audio_->queue(out_data.data(), out_bytes, sdl_err);
#endif
}

//=============================================================================
// Seeking
//=============================================================================

bool FFmpegBackend::flushDecoders() {
  if (video_decoder_) {
    avcodec_flush_buffers(video_decoder_);
  }
  if (audio_decoder_) {
    avcodec_flush_buffers(audio_decoder_);
  }
  if (subtitle_decoder_) {
    avcodec_flush_buffers(subtitle_decoder_);
  }
  // A seek makes the decoder rescan cues it already fed (in both
  // directions); without the flush they would pile up as overlapping
  // duplicates on the libass track.
  if (ass_feed_active_) {
    ass_renderer_.flushEvents();
  }
  return true;
}

bool FFmpegBackend::seekToTimestamp(std::chrono::milliseconds position, bool emit_event) {
  // Convert to stream time base
  int64_t timestamp = AV_NOPTS_VALUE;
  int stream_index = -1;

  if (video_stream_index_ >= 0) {
    stream_index = video_stream_index_;
    AVRational tb = format_ctx_->streams[video_stream_index_]->time_base;
    timestamp = toAVTimestamp(position, tb);
  } else if (audio_stream_index_ >= 0) {
    stream_index = audio_stream_index_;
    AVRational tb = format_ctx_->streams[audio_stream_index_]->time_base;
    timestamp = toAVTimestamp(position, tb);
  }

  if (timestamp == AV_NOPTS_VALUE) {
    return fail("seekToTimestamp: no valid stream for seeking", emit_event);
  }

  // Flush decoders
  flushDecoders();

  // Seek
  int ret = av_seek_frame(format_ctx_, stream_index, timestamp, AVSEEK_FLAG_BACKWARD);
  if (ret < 0) {
    return fail(fmt::format("seekToTimestamp: seek failed: {}", avError(ret)), emit_event);
  }

  // Clear frame queues
  {
    std::lock_guard<std::mutex> vlock(video_queue_mutex_);
    clearDecodedQueue(video_queue_);
  }
  {
    std::lock_guard<std::mutex> alock(audio_queue_mutex_);
    clearDecodedQueue(audio_queue_);
  }

  if (sdl_audio_) {
    sdl_audio_->clear();
  }

  return true;
}

//=============================================================================
// Runtime audio track switching
//=============================================================================

bool FFmpegBackend::audioTrackPending() {
  std::lock_guard<std::mutex> lock(pending_audio_mutex_);
  return pending_audio_track_ >= 0;
}

void FFmpegBackend::applyPendingAudioTrack() {
  AVCodecContext* new_decoder = nullptr;
  int new_track = -1;
  {
    std::lock_guard<std::mutex> lock(pending_audio_mutex_);
    if (pending_audio_track_ < 0) {
      return;
    }
    new_decoder = pending_audio_decoder_;
    new_track = pending_audio_track_;
    pending_audio_decoder_ = nullptr;
    pending_audio_track_ = -1;
  }

  if (!new_decoder) {
    return;
  }

  // Resume at the position playback had reached when the switch was
  // requested. This runs on the decode thread, which never takes
  // decode_mutex_ (close() joins while holding it), so seekToTimestamp
  // executes on its lock-free path by design.
  std::chrono::milliseconds resume_at{0};
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    resume_at = current_position_;
  }

  if (!seekToTimestamp(resume_at, /*emit_event=*/false)) {
    // Keep the current track and decoder; drop the new one.
    avcodec_free_context(&new_decoder);
    {
      std::lock_guard<std::mutex> lock(info_mutex_);
      media_info_.selected_audio = audio_stream_index_;
    }
    fail(fmt::format(
      "selectTrack: failed to seek new audio track {} to the resume position", new_track));
    return;
  }

  avcodec_free_context(&audio_decoder_);
  audio_decoder_ = new_decoder;
  audio_stream_index_ = new_track;
  audio_params_.sample_rate = audio_decoder_->sample_rate;
  audio_params_.channels = audio_decoder_->ch_layout.nb_channels;
  audio_params_.channel_layout = audio_decoder_->ch_layout.u.mask;

  {
    std::lock_guard<std::mutex> lock(info_mutex_);
    media_info_.selected_audio = new_track;
  }
  emit(Event{EventType::MediaInfoChanged});
}

bool FFmpegBackend::subtitleTrackPending() {
  std::lock_guard<std::mutex> lock(pending_subtitle_mutex_);
  return pending_subtitle_track_ >= 0;
}

void FFmpegBackend::applyPendingSubtitleTrack() {
  AVCodecContext* new_decoder = nullptr;
  int new_track = -1;
  {
    std::lock_guard<std::mutex> lock(pending_subtitle_mutex_);
    if (pending_subtitle_track_ < 0) {
      return;
    }
    new_decoder = pending_subtitle_decoder_;
    new_track = pending_subtitle_track_;
    pending_subtitle_decoder_ = nullptr;
    pending_subtitle_track_ = -1;
  }

  if (!new_decoder) {
    return;
  }

  // No resume seek, unlike the audio switch: subtitle cues stream from
  // the decoder's read position and the playhead does not depend on them.
  // This runs on the decode thread, which never takes decode_mutex_
  // (close() joins while holding it), so the members swap lock-free by
  // the same design as applyPendingAudioTrack.
  avcodec_free_context(&subtitle_decoder_);
  subtitle_decoder_ = new_decoder;
  subtitle_stream_index_ = new_track;
  // The gate selectTrack kept closed so no old-stream packet could decode
  // into the freshly armed renderer reopens with the swap.
  subtitle_decode_active_.store(true, std::memory_order_relaxed);
}

AVCodecContext* FFmpegBackend::buildSubtitleDecoder(AVFormatContext* ctx,
                                                    TrackId id,
                                                    const char* what,
                                                    std::string& error) {
  // For the playing context the caller holds decode_mutex_ (selectTrack);
  // for the export pass ctx is the call's own reopened input. The caller
  // reports the error, and on any failure here the old decoder stays put.
  if (id < 0 || id >= static_cast<TrackId>(ctx->nb_streams) ||
      !ctx->streams[id] || !ctx->streams[id]->codecpar ||
      ctx->streams[id]->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
    error = fmt::format("{}: unknown subtitle track id", what);
    return nullptr;
  }
  const AVCodecParameters* codecpar = ctx->streams[id]->codecpar;
  const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
  if (!codec) {
    error = fmt::format("{}: subtitle codec not found", what);
    return nullptr;
  }
  AVCodecContext* decoder = avcodec_alloc_context3(codec);
  if (!decoder) {
    error = fmt::format("{}: failed to allocate subtitle decoder context", what);
    return nullptr;
  }
  int ret = avcodec_parameters_to_context(decoder, codecpar);
  if (ret < 0) {
    avcodec_free_context(&decoder);
    error = fmt::format("{}: failed to copy subtitle params: {}", what, avError(ret));
    return nullptr;
  }
  ret = avcodec_open2(decoder, codec, nullptr);
  if (ret < 0) {
    avcodec_free_context(&decoder);
    error = fmt::format("{}: failed to open subtitle decoder: {}", what, avError(ret));
    return nullptr;
  }
  return decoder;
}

void FFmpegBackend::reportStoppedPlayback() {
  bool notify = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (playback_state_ != PlaybackState::Stopped) {
      playback_state_ = PlaybackState::Stopped;
      notify = true;
    }
  }
  if (notify) {
    emit(Event{EventType::StateChanged});
  }
}

//=============================================================================
// Utility Functions
//=============================================================================

std::string FFmpegBackend::getCodecName(AVCodecContext* ctx) {
  if (!ctx || !ctx->codec) {
    return "unknown";
  }
  return ctx->codec->name;
}

std::chrono::milliseconds FFmpegBackend::fromAVTimestamp(int64_t pts, int time_base_num, int time_base_den) {
  if (pts == AV_NOPTS_VALUE) {
    return std::chrono::milliseconds(0);
  }
  // Convert to milliseconds: pts * time_base * 1000
  int64_t ms = (pts * 1000 * time_base_num) / time_base_den;
  return std::chrono::milliseconds(ms);
}

int64_t FFmpegBackend::toAVTimestamp(std::chrono::milliseconds ms, AVRational time_base) {
  // Convert from milliseconds: ms / 1000 / time_base
  return static_cast<int64_t>((ms.count() / 1000.0) * time_base.den / time_base.num);
}

std::string FFmpegBackend::avError(int errnum) {
  char errbuf[AV_ERROR_MAX_STRING_SIZE];
  av_strerror(errnum, errbuf, sizeof(errbuf));
  return std::string(errbuf);
}

//=============================================================================
// Factory Function
//=============================================================================

std::unique_ptr<IBackend> makeFFmpegBackend() {
  return std::make_unique<FFmpegBackend>();
}

} // namespace soar
