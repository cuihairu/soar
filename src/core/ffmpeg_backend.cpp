#include "soar/core/ffmpeg_backend.h"

#include <fmt/format.h>

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
#include <cstring>
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
std::string assDialogueText(const char* ass) {
  if (ass == nullptr) {
    return {};
  }
  const std::string_view s(ass);
  std::string out;
  std::size_t pos = 0;
  while (pos < s.size()) {
    const std::size_t eol = s.find('\n', pos);
    const std::size_t line_end = eol == std::string_view::npos ? s.size() : eol;
    const std::string_view line = s.substr(pos, line_end - pos);
    pos = eol == std::string_view::npos ? s.size() : eol + 1;
    if (line.rfind("Dialogue:", 0) != 0) {
      continue;
    }
    int commas = 0;
    std::size_t i = 0;
    while (i < line.size() && commas < 9) {
      if (line[i] == ',') {
        ++commas;
      }
      ++i;
    }
    if (commas < 9) {
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
      out.push_back(c);
    }
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '\r')) {
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

  bool ensureOpen(int target_rate, int target_channels, std::string& err) {
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
      if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        err = SDL_GetError();
        return false;
      }
    }

    if (dev != 0 && sample_rate == target_rate && channels == target_channels) {
      return true;
    }

    close();

    SDL_AudioSpec wanted{};
    wanted.freq = target_rate;
    wanted.format = AUDIO_F32SYS;
    wanted.channels = static_cast<Uint8>(std::clamp(target_channels, 1, 8));
    wanted.samples = 2048;
    wanted.callback = nullptr; // queued audio

    dev = SDL_OpenAudioDevice(nullptr, 0, &wanted, &obtained, SDL_AUDIO_ALLOW_FORMAT_CHANGE);
    if (dev == 0) {
      err = SDL_GetError();
      return false;
    }

    sample_rate = obtained.freq;
    channels = obtained.channels;
    SDL_PauseAudioDevice(dev, 0);
    return true;
  }

  void clear() {
    if (dev != 0) {
      SDL_ClearQueuedAudio(dev);
    }
  }

  void close() {
    if (dev != 0) {
      SDL_CloseAudioDevice(dev);
      dev = 0;
    }
    sample_rate = 0;
    channels = 0;
    obtained = SDL_AudioSpec{};
  }

  std::size_t queuedBytes() const {
    if (dev == 0) {
      return 0;
    }
    return SDL_GetQueuedAudioSize(dev);
  }

  bool queue(const void* data, std::size_t bytes, std::string& err) {
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
  bool ensureOpen(int, int, std::string&) { return false; }
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

    // Drop any audio-track switch that never reached the decode loop.
    {
      std::lock_guard<std::mutex> plock(pending_audio_mutex_);
      if (pending_audio_decoder_) {
        avcodec_free_context(&pending_audio_decoder_);
        pending_audio_decoder_ = nullptr;
      }
      pending_audio_track_ = -1;
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

  {
    std::lock_guard<std::mutex> lock(video_frame_mutex_);
    video_frame_ready_ = false;
    latest_video_frame_ = DecodedVideoFrame{};
    staging_video_frame_ = DecodedVideoFrame{};
  }

  {
    std::lock_guard<std::mutex> lock(subtitle_frame_mutex_);
    subtitle_frame_ready_ = false;
    latest_subtitle_frame_ = DecodedSubtitleFrame{};
  }

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
  std::lock_guard<std::mutex> lock(subtitle_frame_mutex_);
  if (!subtitle_frame_ready_) {
    return false;
  }
  out = latest_subtitle_frame_;
  subtitle_frame_ready_ = false;
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

  {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    if (!format_ctx_) {
      return fail("selectTrack: no media opened");
    }
  }

  if (type == TrackType::Video) {
    return fail("selectTrack: video track switching not supported");
  }

  // Subtitle selection is metadata only (no subtitle decoder yet), so it is
  // safe in any state.
  if (type == TrackType::Subtitle) {
    std::string error;
    {
      std::lock_guard<std::mutex> lock(decode_mutex_);
      if (id < 0 || id >= static_cast<TrackId>(format_ctx_->nb_streams)) {
        error = "selectTrack: unknown subtitle track id";
      } else if (!format_ctx_->streams[id] || !format_ctx_->streams[id]->codecpar ||
                 format_ctx_->streams[id]->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
        error = "selectTrack: unknown subtitle track id";
      }
    }

    if (!error.empty()) {
      return fail(std::move(error));
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
  emit(Event{EventType::MediaInfoChanged});
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
               audioTrackPending();
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
        subtitle_frame_ready_ = false;
        latest_subtitle_frame_ = DecodedSubtitleFrame{};
      }
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
        // End of file
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
    } else if (packet->stream_index == subtitle_stream_index_) {
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

      std::fprintf(stderr, "SUBPROBE ret=%d got=%d rects=%u type=%d ass=[%s]\n", ret, got_sub, sub.num_rects, got_sub && sub.num_rects ? (int)sub.rects[0]->type : -1, got_sub && sub.num_rects ? sub.rects[0]->ass : nullptr);
      if (got_sub != 0) {
        const AVStream* stream = format_ctx_->streams[subtitle_stream_index_];
        const int64_t ts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
        auto pts = fromAVTimestamp(
          ts,
          stream->time_base.num,
          stream->time_base.den
        );
        const auto duration = std::chrono::milliseconds(
          sub.end_display_time > sub.start_display_time
              ? sub.end_display_time - sub.start_display_time
              : 0
        );
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
  latest_subtitle_frame_ = sub;
  subtitle_frame_ready_ = true;
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

  if (!text.empty()) {
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
  if (!sdl_audio_->ensureOpen(target_rate, in_channels, sdl_err)) {
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
