#include "soar/core/ffmpeg_backend.h"

#include <fmt/format.h>

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

#ifdef SOAR_WITH_SDL2
#  include <SDL.h>
#endif

namespace soar {
namespace {

constexpr std::size_t kMaxVideoQueueFrames = 6;
constexpr std::size_t kMaxAudioQueueFrames = 32;
constexpr auto kPositionEmitGranularity = std::chrono::milliseconds(200);

int ffmpegInterruptCallback(void* opaque) {
  auto* stop_flag = static_cast<std::atomic<bool>*>(opaque);
  if (!stop_flag) {
    return 0;
  }
  return stop_flag->load() ? 1 : 0;
}

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
  close();

  // Ensure decode thread is stopped
  if (decode_thread_.joinable()) {
    should_stop_decoding_ = true;
    decode_cv_.notify_all();
    decode_thread_.join();
  }

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

void FFmpegBackend::emit(Event e) {
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

  e.state = state;
  e.position = position;
  sink->onEvent(e);
}

bool FFmpegBackend::fail(std::string message) {
  {
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_ = std::move(message);
  }
  emit(Event{EventType::Error, PlaybackState::Stopped, std::chrono::milliseconds(0), last_error_});
  return false;
}

bool FFmpegBackend::fatal(std::string message) {
  {
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_ = std::move(message);
  }

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    playback_state_ = PlaybackState::Error;
  }

  emit(Event{EventType::Error, PlaybackState::Error, std::chrono::milliseconds(0), last_error_});
  emit(Event{EventType::StateChanged});
  return false;
}

//=============================================================================
// IBackend implementation - Media Open/Close
//=============================================================================

bool FFmpegBackend::open(const MediaSource& source) {
  // Close any existing media
  close();

  // Reset state
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    playback_state_ = PlaybackState::Stopped;
    current_position_ = std::chrono::milliseconds(0);
    last_emitted_position_ = std::chrono::milliseconds(0);
    clock_origin_ = std::chrono::steady_clock::time_point{};
  }

  // Open the media file
  if (!openContext(source.uri)) {
    return false;
  }

  // Find and initialize stream info
  if (!findStreamInfo()) {
    closeContext();
    return false;
  }

  // Setup decoders for video/audio streams
  if (!setupDecoders()) {
    cleanupDecoders();
    closeContext();
    return false;
  }

  // Build MediaInfo
  {
    std::lock_guard<std::mutex> lock(info_mutex_);

    media_info_ = MediaInfo{};

    media_info_.duration = std::chrono::milliseconds(
      static_cast<int64_t>(format_ctx_->duration * 1000.0 / AV_TIME_BASE)
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
  }

  // Emit events
  emit(Event{EventType::MediaInfoChanged});
  emit(Event{EventType::StateChanged});

  return true;
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
    current_position_ = std::chrono::milliseconds(0);
  }

  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    playback_state_ = PlaybackState::Stopped;
    clock_origin_ = std::chrono::steady_clock::time_point{};
    last_emitted_position_ = std::chrono::milliseconds(0);
  }

  {
    std::lock_guard<std::mutex> lock(video_frame_mutex_);
    video_frame_ready_ = false;
    latest_video_frame_ = DecodedVideoFrame{};
    staging_video_frame_ = DecodedVideoFrame{};
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
  if (format_ctx_ == nullptr) {
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
  if (format_ctx_ == nullptr) {
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

  flushDecoders();

  emit(Event{EventType::StateChanged});
  emit(Event{EventType::PositionChanged});
  return true;
}

bool FFmpegBackend::seek(std::chrono::milliseconds position) {
  if (format_ctx_ == nullptr) {
    return fail("seek: no media opened");
  }

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

  // Clamp position to valid range
  std::chrono::milliseconds clamped = std::clamp(
    position,
    std::chrono::milliseconds(0),
    duration
  );

  bool has_decode_thread = false;
  {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    if ((state == PlaybackState::Ended || state == PlaybackState::Error) && decode_thread_.joinable()) {
      decode_thread_.join();
    }
    has_decode_thread = decode_thread_.joinable() && (state == PlaybackState::Playing || state == PlaybackState::Paused);
  }

  if (!has_decode_thread) {
    if (!seekToTimestamp(clamped)) {
      return false;
    }

    bool state_changed = false;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
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

    emit(Event{EventType::PositionChanged});
    if (state_changed) {
      emit(Event{EventType::StateChanged});
    }
    return true;
  }

  // Store seek request for decode thread
  seek_requested_ = true;
  seek_target_ = clamped.count();

  decode_cv_.notify_all();
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

  // For now, only allow switching during initialization
  // Runtime track switching requires more complex state management
  if (state != PlaybackState::Stopped) {
    return fail("selectTrack: track switching only supported when stopped");
  }

  bool changed = false;
  std::string error;
  if (type == TrackType::Video) {
    error = "selectTrack: video track switching not supported";
  } else if (type == TrackType::Audio) {
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
          AVCodecContext* new_decoder = avcodec_alloc_context3(codec);
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
              } else {
                avcodec_free_context(&audio_decoder_);
                audio_decoder_ = new_decoder;
                audio_stream_index_ = id;
                audio_params_.sample_rate = audio_decoder_->sample_rate;
                audio_params_.channels = audio_decoder_->ch_layout.nb_channels;
                audio_params_.channel_layout = audio_decoder_->ch_layout.u.mask;
              }
            }
          }
        }
      }
    }

    if (error.empty()) {
      std::lock_guard<std::mutex> lock(info_mutex_);
      media_info_.selected_audio = id;
      changed = true;
    }
  } else if (type == TrackType::Subtitle) {
    {
      std::lock_guard<std::mutex> lock(decode_mutex_);
      if (id < 0 || id >= static_cast<TrackId>(format_ctx_->nb_streams)) {
        error = "selectTrack: unknown subtitle track id";
      } else if (!format_ctx_->streams[id] || !format_ctx_->streams[id]->codecpar ||
                 format_ctx_->streams[id]->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
        error = "selectTrack: unknown subtitle track id";
      }
    }

    if (error.empty()) {
      std::lock_guard<std::mutex> lock(info_mutex_);
      media_info_.selected_subtitle = id;
      changed = true;
    }
  } else {
    error = "selectTrack: invalid track type";
  }

  if (!error.empty()) {
    return fail(std::move(error));
  }

  if (changed) {
    emit(Event{EventType::MediaInfoChanged});
  }
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

bool FFmpegBackend::openContext(const std::string& uri) {
  AVFormatContext* ctx = avformat_alloc_context();
  if (!ctx) {
    return fatal("open: failed to allocate format context");
  }

  ctx->interrupt_callback.callback = &ffmpegInterruptCallback;
  ctx->interrupt_callback.opaque = &should_stop_decoding_;

  int ret = avformat_open_input(&ctx, uri.c_str(), nullptr, nullptr);
  if (ret < 0) {
    avformat_free_context(ctx);
    return fatal(fmt::format("open: failed to open '{}': {}", uri, avError(ret)));
  }

  format_ctx_ = ctx;
  return true;
}

void FFmpegBackend::closeContext() {
  if (format_ctx_) {
    avformat_close_input(&format_ctx_);
    format_ctx_ = nullptr;
  }

  video_stream_index_ = -1;
  audio_stream_index_ = -1;
}

bool FFmpegBackend::findStreamInfo() {
  int ret = avformat_find_stream_info(format_ctx_, nullptr);
  if (ret < 0) {
    return fatal(fmt::format("findStreamInfo: failed: {}", avError(ret)));
  }

  video_stream_index_ = pickBestStreamIndex(format_ctx_, AVMEDIA_TYPE_VIDEO);
  audio_stream_index_ = pickBestStreamIndex(format_ctx_, AVMEDIA_TYPE_AUDIO);

  if (video_stream_index_ < 0 && audio_stream_index_ < 0) {
    return fatal("findStreamInfo: no video or audio stream found");
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
      return fatal(fmt::format("setupDecoders: video codec not found: {}", static_cast<int>(codecpar->codec_id)));
    }

    video_decoder_ = avcodec_alloc_context3(codec);
    if (!video_decoder_) {
      return fatal("setupDecoders: failed to allocate video decoder context");
    }

    int ret = avcodec_parameters_to_context(video_decoder_, codecpar);
    if (ret < 0) {
      return fatal(fmt::format("setupDecoders: failed to copy video params: {}", avError(ret)));
    }

    ret = avcodec_open2(video_decoder_, codec, nullptr);
    if (ret < 0) {
      return fatal(fmt::format("setupDecoders: failed to open video decoder: {}", avError(ret)));
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
      return fatal(fmt::format("setupDecoders: audio codec not found: {}", static_cast<int>(codecpar->codec_id)));
    }

    audio_decoder_ = avcodec_alloc_context3(codec);
    if (!audio_decoder_) {
      return fatal("setupDecoders: failed to allocate audio decoder context");
    }

    int ret = avcodec_parameters_to_context(audio_decoder_, codecpar);
    if (ret < 0) {
      return fatal(fmt::format("setupDecoders: failed to copy audio params: {}", avError(ret)));
    }

    ret = avcodec_open2(audio_decoder_, codec, nullptr);
    if (ret < 0) {
      return fatal(fmt::format("setupDecoders: failed to open audio decoder: {}", avError(ret)));
    }

    // Store audio parameters (FFmpeg 8.0+ uses ch_layout)
    audio_params_.sample_rate = audio_decoder_->sample_rate;
    audio_params_.channels = audio_decoder_->ch_layout.nb_channels;
    // Store channel layout as uint64_t for compatibility
    audio_params_.channel_layout = audio_decoder_->ch_layout.u.mask;
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
  while (!should_stop_decoding_) {
    // Check for pause/stop
    {
      std::unique_lock<std::mutex> lock(state_mutex_);
      decode_cv_.wait(lock, [this] {
        return should_stop_decoding_ ||
               playback_state_ == PlaybackState::Playing ||
               seek_requested_.load();
      });
    }

    if (should_stop_decoding_) {
      break;
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
    }

    // Read packet
    int ret = av_read_frame(format_ctx_, packet);
    if (ret < 0) {
      if (ret == AVERROR_EXIT) {
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
    }

    av_packet_unref(packet);
    drainFrameQueues();

    if (fatal_decode_error) {
      break;
    }
  }

  av_frame_free(&frame);
  av_packet_free(&packet);
}

bool FFmpegBackend::decodeVideoFrame(AVFrame* frame) {
  // Video decoding is handled in decodeLoop()
  return true;
}

bool FFmpegBackend::decodeAudioFrame(AVFrame* frame) {
  // Audio decoding is handled in decodeLoop()
  return true;
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
  return true;
}

bool FFmpegBackend::seekToTimestamp(std::chrono::milliseconds position) {
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
    return fail("seekToTimestamp: no valid stream for seeking");
  }

  // Flush decoders
  flushDecoders();

  // Seek
  int ret = av_seek_frame(format_ctx_, stream_index, timestamp, AVSEEK_FLAG_BACKWARD);
  if (ret < 0) {
    return fail(fmt::format("seekToTimestamp: seek failed: {}", avError(ret)));
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
