#include "soar/core/ffmpeg_backend.h"

#include <fmt/format.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libavutil/timestamp.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <chrono>
#include <cstring>

namespace soar {

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
  std::lock_guard<std::mutex> lock(event_mutex_);
  if (event_sink_) {
    std::lock_guard<std::mutex> state_lock(state_mutex_);
    e.state = playback_state_;
    e.position = current_position_;
    event_sink_->onEvent(e);
  }
}

bool FFmpegBackend::fail(std::string message) {
  {
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_ = std::move(message);
  }
  emit(Event{EventType::Error, playback_state_, current_position_, last_error_});
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
    start_time_ = std::chrono::milliseconds(0);
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

    media_info_.duration = std::chrono::milliseconds(
      static_cast<int64_t>(format_ctx_->duration * 1000.0 / AV_TIME_BASE)
    );
    media_info_.seekable = (format_ctx_->iformat->flags & AVFMT_SEEK_TO_PTS) != 0;

    // Add track info
    if (video_stream_index_ >= 0) {
      AVStream* stream = format_ctx_->streams[video_stream_index_];
      media_info_.tracks.push_back({
        video_stream_index_,
        TrackType::Video,
        getCodecName(video_decoder_),
        "",  // language not available for video
        "Video",
        true  // default
      });
      media_info_.selected_video = video_stream_index_;
    }

    if (audio_stream_index_ >= 0) {
      AVStream* stream = format_ctx_->streams[audio_stream_index_];
      media_info_.tracks.push_back({
        audio_stream_index_,
        TrackType::Audio,
        getCodecName(audio_decoder_),
        "und",  // TODO: extract from metadata
        "Audio",
        true
      });
      media_info_.selected_audio = audio_stream_index_;
    }

    media_info_.selected_subtitle = -1;
  }

  // Emit events
  emit(Event{EventType::MediaInfoChanged});
  emit(Event{EventType::StateChanged});

  return true;
}

void FFmpegBackend::close() {
  // Stop decode thread if running
  if (decode_thread_.joinable()) {
    should_stop_decoding_ = true;
    decode_cv_.notify_all();
    decode_thread_.join();
  }

  cleanupDecoders();
  closeContext();

  // Clear queues
  {
    std::lock_guard<std::mutex> vlock(video_queue_mutex_);
    while (!video_queue_.empty()) {
      video_queue_.pop();
    }
  }
  {
    std::lock_guard<std::mutex> alock(audio_queue_mutex_);
    while (!audio_queue_.empty()) {
      audio_queue_.pop();
    }
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
    start_time_ = std::chrono::milliseconds(0);
  }

  emit(Event{EventType::MediaInfoChanged});
  emit(Event{EventType::StateChanged});
}

//=============================================================================
// IBackend implementation - Playback Control
//=============================================================================

bool FFmpegBackend::play() {
  std::lock_guard<std::mutex> lock(state_mutex_);

  if (playback_state_ == PlaybackState::Playing) {
    return true;  // Already playing
  }

  if (format_ctx_ == nullptr) {
    return fail("play: no media opened");
  }

  // Start decode thread if not running
  if (!decode_thread_.joinable()) {
    should_stop_decoding_ = false;
    decode_thread_ = std::thread(&FFmpegBackend::decodeLoop, this);
  }

  playback_state_ = PlaybackState::Playing;

  // Reset start time for synchronization
  start_time_ = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now().time_since_epoch()
  ) - current_position_;

  emit(Event{EventType::StateChanged});
  return true;
}

bool FFmpegBackend::pause() {
  std::lock_guard<std::mutex> lock(state_mutex_);

  if (playback_state_ == PlaybackState::Paused) {
    return true;  // Already paused
  }

  if (format_ctx_ == nullptr) {
    return fail("pause: no media opened");
  }

  playback_state_ = PlaybackState::Paused;
  emit(Event{EventType::StateChanged});
  return true;
}

bool FFmpegBackend::stop() {
  std::lock_guard<std::mutex> lock(state_mutex_);

  if (playback_state_ == PlaybackState::Stopped) {
    return true;  // Already stopped
  }

  if (format_ctx_ == nullptr) {
    return fail("stop: no media opened");
  }

  // Stop decode thread
  if (decode_thread_.joinable()) {
    should_stop_decoding_ = true;
    decode_cv_.notify_all();
    decode_thread_.join();
  }

  playback_state_ = PlaybackState::Stopped;
  current_position_ = std::chrono::milliseconds(0);
  start_time_ = std::chrono::milliseconds(0);

  // Flush decoders
  flushDecoders();

  emit(Event{EventType::StateChanged});
  emit(Event{EventType::PositionChanged});
  return true;
}

bool FFmpegBackend::seek(std::chrono::milliseconds position) {
  std::lock_guard<std::mutex> lock(state_mutex_);

  if (format_ctx_ == nullptr) {
    return fail("seek: no media opened");
  }

  // Clamp position to valid range
  std::chrono::milliseconds clamped = std::clamp(
    position,
    std::chrono::milliseconds(0),
    media_info_.duration
  );

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

  playback_rate_ = rate;
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

//=============================================================================
// IBackend implementation - Track Selection
//=============================================================================

bool FFmpegBackend::selectTrack(TrackType type, TrackId id) {
  std::lock_guard<std::mutex> lock(info_mutex_);

  // For now, only allow switching during initialization
  // Runtime track switching requires more complex state management
  if (playback_state_ != PlaybackState::Stopped) {
    return fail("selectTrack: track switching only supported when stopped");
  }

  if (type == TrackType::Video) {
    // Video switching not supported
    return fail("selectTrack: video track switching not supported");
  }

  if (type == TrackType::Audio) {
    auto it = std::find_if(
      media_info_.tracks.begin(),
      media_info_.tracks.end(),
      [&](const TrackInfo& t) { return t.type == type && t.id == id; }
    );
    if (it == media_info_.tracks.end()) {
      return fail("selectTrack: unknown audio track id");
    }
    media_info_.selected_audio = id;
    emit(Event{EventType::MediaInfoChanged});
    return true;
  }

  if (type == TrackType::Subtitle) {
    // Subtitle support to be implemented
    return fail("selectTrack: subtitles not yet supported");
  }

  return fail("selectTrack: invalid track type");
}

bool FFmpegBackend::disableSubtitles() {
  std::lock_guard<std::mutex> lock(info_mutex_);
  media_info_.selected_subtitle = -1;
  emit(Event{EventType::MediaInfoChanged});
  return true;
}

//=============================================================================
// FFmpeg Context Management
//=============================================================================

bool FFmpegBackend::openContext(const std::string& uri) {
  int ret = avformat_open_input(&format_ctx_, uri.c_str(), nullptr, nullptr);
  if (ret < 0) {
    return fail(fmt::format("open: failed to open '{}': {}", uri, avError(ret)));
  }

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
    return fail(fmt::format("findStreamInfo: failed: {}", avError(ret)));
  }

  // Find video stream
  for (unsigned int i = 0; i < format_ctx_->nb_streams; i++) {
    if (format_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
        video_stream_index_ < 0) {
      video_stream_index_ = static_cast<int>(i);
      break;
    }
  }

  // Find audio stream
  for (unsigned int i = 0; i < format_ctx_->nb_streams; i++) {
    if (format_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
        audio_stream_index_ < 0) {
      audio_stream_index_ = static_cast<int>(i);
      break;
    }
  }

  if (video_stream_index_ < 0 && audio_stream_index_ < 0) {
    return fail("findStreamInfo: no video or audio stream found");
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
      return fail(fmt::format("setupDecoders: video codec not found: {}", static_cast<int>(codecpar->codec_id)));
    }

    video_decoder_ = avcodec_alloc_context3(codec);
    if (!video_decoder_) {
      return fail("setupDecoders: failed to allocate video decoder context");
    }

    int ret = avcodec_parameters_to_context(video_decoder_, codecpar);
    if (ret < 0) {
      return fail(fmt::format("setupDecoders: failed to copy video params: {}", avError(ret)));
    }

    ret = avcodec_open2(video_decoder_, codec, nullptr);
    if (ret < 0) {
      return fail(fmt::format("setupDecoders: failed to open video decoder: {}", avError(ret)));
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
      return fail(fmt::format("setupDecoders: audio codec not found: {}", static_cast<int>(codecpar->codec_id)));
    }

    audio_decoder_ = avcodec_alloc_context3(codec);
    if (!audio_decoder_) {
      return fail("setupDecoders: failed to allocate audio decoder context");
    }

    int ret = avcodec_parameters_to_context(audio_decoder_, codecpar);
    if (ret < 0) {
      return fail(fmt::format("setupDecoders: failed to copy audio params: {}", avError(ret)));
    }

    ret = avcodec_open2(audio_decoder_, codec, nullptr);
    if (ret < 0) {
      return fail(fmt::format("setupDecoders: failed to open audio decoder: {}", avError(ret)));
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
}

//=============================================================================
// Decoding Loop
//=============================================================================

void FFmpegBackend::decodeLoop() {
  AVPacket* packet = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();

  if (!packet || !frame) {
    fail("decodeLoop: failed to allocate packet/frame");
    return;
  }

  while (!should_stop_decoding_) {
    // Check for pause
    {
      std::unique_lock<std::mutex> lock(state_mutex_);
      if (playback_state_ != PlaybackState::Playing) {
        decode_cv_.wait(lock, [this] {
          return playback_state_ == PlaybackState::Playing || should_stop_decoding_;
        });
      }
    }

    if (should_stop_decoding_) {
      break;
    }

    // Handle seek request
    if (seek_requested_) {
      auto target = std::chrono::milliseconds(seek_target_.load());
      seek_requested_ = false;

      if (seekToTimestamp(target)) {
        // Update position
        std::lock_guard<std::mutex> lock(state_mutex_);
        current_position_ = target;
        emit(Event{EventType::PositionChanged});
      }
    }

    // Read packet
    int ret = av_read_frame(format_ctx_, packet);
    if (ret < 0) {
      if (ret == AVERROR_EOF) {
        // End of file
        std::lock_guard<std::mutex> lock(state_mutex_);
        playback_state_ = PlaybackState::Ended;
        emit(Event{EventType::StateChanged});
        break;
      }
      // Error
      fail(fmt::format("decodeLoop: av_read_frame failed: {}", avError(ret)));
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
          fail(fmt::format("decodeLoop: video decode failed: {}", avError(ret)));
          break;
        }

        // Process video frame
        auto pts = fromAVTimestamp(
          frame->pts,
          format_ctx_->streams[video_stream_index_]->time_base.num,
          format_ctx_->streams[video_stream_index_]->time_base.den
        );

        queueVideoFrame(frame, pts);
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
          fail(fmt::format("decodeLoop: audio decode failed: {}", avError(ret)));
          break;
        }

        // Process audio frame
        auto pts = fromAVTimestamp(
          frame->pts,
          format_ctx_->streams[audio_stream_index_]->time_base.num,
          format_ctx_->streams[audio_stream_index_]->time_base.den
        );

        queueAudioFrame(frame, pts);
      }
    }

    av_packet_unref(packet);
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
  // Clone the frame for queue storage
  AVFrame* cloned = av_frame_clone(frame);
  if (!cloned) {
    return;
  }

  DecodedFrame decoded{cloned, pts};

  std::lock_guard<std::mutex> lock(video_queue_mutex_);
  video_queue_.push(decoded);
}

void FFmpegBackend::queueAudioFrame(AVFrame* frame, std::chrono::milliseconds pts) {
  // Clone the frame for queue storage
  AVFrame* cloned = av_frame_clone(frame);
  if (!cloned) {
    return;
  }

  DecodedFrame decoded{cloned, pts};

  std::lock_guard<std::mutex> lock(audio_queue_mutex_);
  audio_queue_.push(decoded);
}

void FFmpegBackend::processVideoFrame(const DecodedFrame& frame) {
  // Update position
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    current_position_ = frame.pts;
  }
  emit(Event{EventType::PositionChanged});

  // TODO: Render frame (to be implemented with SDL2/OpenGL)
}

void FFmpegBackend::processAudioFrame(const DecodedFrame& frame) {
  // TODO: Play audio (to be implemented with SDL2/PortAudio)
}

void FFmpegBackend::renderVideoFrame(const AVFrame* frame) {
  // TODO: Implement video rendering
}

void FFmpegBackend::playAudioFrame(const AVFrame* frame) {
  // TODO: Implement audio playback
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
    while (!video_queue_.empty()) {
      video_queue_.pop();
    }
  }
  {
    std::lock_guard<std::mutex> alock(audio_queue_mutex_);
    while (!audio_queue_.empty()) {
      audio_queue_.pop();
    }
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
