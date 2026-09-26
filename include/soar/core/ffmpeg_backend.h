#pragma once

#include "soar/core/backend.h"

#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

extern "C" {
// Forward declarations for FFmpeg types
struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct SwrContext;
struct SwsContext;
struct AVRational;
}

namespace soar {

/**
 * FFmpeg-based media backend implementation.
 *
 * This backend uses FFmpeg libraries to decode and play media files.
 * It supports most common video/audio formats and codecs.
 *
 * Implementation notes:
 * - Uses separate decoding thread for async operation
 * - Software decoding (hardware acceleration can be added later)
 * - Basic audio/video synchronization using PTS
 *
 * @threadsafe All public methods are thread-safe
 */
class FFmpegBackend : public IBackend {
public:
  struct DecodedVideoFrame {
    int width{0};
    int height{0};
    int stride_y{0};
    int stride_u{0};
    int stride_v{0};
    std::vector<std::uint8_t> y;
    std::vector<std::uint8_t> u;
    std::vector<std::uint8_t> v;
    std::chrono::milliseconds pts{0};
  };

  struct DecodedSubtitleFrame {
    std::string text;
    std::chrono::milliseconds pts{0};
    std::chrono::milliseconds duration{0};
  };

  FFmpegBackend();
  ~FFmpegBackend() override;

  // IBackend implementation
  void setEventSink(IEventSink* sink) override;
  bool open(const MediaSource& source) override;
  void close() override;

  bool play() override;
  bool pause() override;
  bool stop() override;

  bool seek(std::chrono::milliseconds position) override;
  bool setRate(double rate) override;
  bool setVolume(double volume01) override;
  bool setMuted(bool muted) override;

  MediaInfo mediaInfo() const override;
  std::chrono::milliseconds position() const override;

  bool selectTrack(TrackType type, TrackId id) override;
  bool disableSubtitles() override;

  PlaybackState state() const override;
  std::string lastError() const override;

  // Optional: allow the app to pull the latest decoded video frame (YUV420P).
  // Thread-safe; returns true only when a new frame is available.
  bool tryGetVideoFrame(DecodedVideoFrame& out);

  // Optional: allow the app to pull the latest decoded subtitle frame.
  // Thread-safe; returns true only when a new frame is available.
  bool tryGetSubtitleFrame(DecodedSubtitleFrame& out);

private:
  // Internal types
  struct AudioParams {
    int sample_rate;
    int channels;
    uint64_t channel_layout;
  };

  struct VideoParams {
    int width;
    int height;
    int pix_fmt;
  };

  struct DecodedFrame {
    AVFrame* frame;
    std::chrono::milliseconds pts;
  };

  // FFmpeg context management
  // openContext does not publish format_ctx_ itself; the caller assigns it
  // under decode_mutex_ once the whole open path succeeded.
  bool openContext(const MediaSource& source, AVFormatContext** out_ctx);
  void closeContext();
  bool findStreamInfo();
  bool setupDecoders();
  void cleanupDecoders();

  // Decoding
  void decodeLoop();
  void queueVideoFrame(AVFrame* frame, std::chrono::milliseconds pts);
  void queueAudioFrame(AVFrame* frame, std::chrono::milliseconds pts);
  void queueSubtitleFrame(const std::string& text, std::chrono::milliseconds pts, std::chrono::milliseconds duration);
  void drainFrameQueues();

  // Frame processing
  void processVideoFrame(DecodedFrame frame);
  void processAudioFrame(DecodedFrame frame);
  void processSubtitleFrame(AVFrame* frame, std::chrono::milliseconds pts, std::chrono::milliseconds duration);
  bool waitForPresentationTime(std::chrono::milliseconds pts);

  // Rendering (to be implemented)
  void renderVideoFrame(const AVFrame* frame);
  void playAudioFrame(const AVFrame* frame);

  // Seeking
  bool flushDecoders();
  // emit_event=false only records the error (for callers that must emit
  // outside decode_mutex_ to keep event callbacks re-entrancy safe).
  bool seekToTimestamp(std::chrono::milliseconds position, bool emit_event = true);

  // Runtime audio track switching: selectTrack() hands a freshly built
  // decoder to the decode thread through pending_audio_*; the decode
  // thread installs it at a safe point (applyPendingAudioTrack) and seeks
  // back to the current position so playback continues seamlessly.
  bool audioTrackPending();
  void applyPendingAudioTrack();

  // Utility functions
  static std::string getCodecName(AVCodecContext* ctx);
  static std::chrono::milliseconds fromAVTimestamp(int64_t pts, int time_base_num, int time_base_den);
  static int64_t toAVTimestamp(std::chrono::milliseconds ms, AVRational time_base);
  std::string avError(int errnum);

  // Interrupt callback installed on every AVFormatContext (openContext).
  // Runs on the decode thread inside FFmpeg's IO poll loop: honors stop(),
  // and while a media read is in flight doubles as the network stall
  // watchdog — reports buffering, aborts the read past the tolerance
  // window (decodeLoop tells that abort apart from stop()).
  static int ffmpegInterruptCallback(void* opaque);

  // Disk-cache AVIO shim (--cache-dir + http:// only, see MediaSource).
  // AvioCacheContext owns the HttpCache plus the logical read position;
  // its AVIOContext/buffer are freed by closeContext (avio_context_free
  // releases the buffer itself — do not av_free() it a second time).
  struct AvioCacheContext;
  // True only while decodeLoop is running. The download-progress emit in
  // avioReadCallback keys off this so progress is reported only from the
  // decode thread: a stopped-state seek fills its hole blocks while holding
  // decode_mutex_ (see seek()), and emitting from there would break the
  // "events go out after the lock is released" invariant that lets sink
  // callbacks re-enter the public API.
  std::atomic<bool> decode_loop_running_{false};
  static int avioReadCallback(void* opaque, uint8_t* buf, int buf_size);
  static int64_t avioSeekCallback(void* opaque, int64_t offset, int whence);

  // Event emission. emit_event=false records the error/state without
  // emitting, for code paths that already hold decode_mutex_ (events are
  // always emitted outside that lock so user callbacks may re-enter).
  bool hasMedia();
  void emit(const Event& e);
  bool fail(std::string message, bool emit_event = true);
  bool fatal(std::string message, bool emit_event = true);

  // Member variables

  // Event system
  mutable std::mutex event_mutex_;
  IEventSink* event_sink_{nullptr};

  // Video frame handoff (for UI rendering on main thread)
  mutable std::mutex video_frame_mutex_;
  bool video_frame_ready_{false};
  DecodedVideoFrame latest_video_frame_{};
  DecodedVideoFrame staging_video_frame_{};

  // Subtitle frame handoff (for UI rendering on main thread)
  mutable std::mutex subtitle_frame_mutex_;
  bool subtitle_frame_ready_{false};
  DecodedSubtitleFrame latest_subtitle_frame_{};

  // FFmpeg contexts
  AVFormatContext* format_ctx_{nullptr};
  AVCodecContext* video_decoder_{nullptr};
  AVCodecContext* audio_decoder_{nullptr};
  AVCodecContext* subtitle_decoder_{nullptr};

  // Disk-cache AVIO state; non-null only while a cached http:// source is
  // open (openContext builds it, closeContext tears it down).
  std::unique_ptr<AvioCacheContext> avio_cache_;

  int video_stream_index_{-1};
  int audio_stream_index_{-1};
  int subtitle_stream_index_{-1};

  // Rescalers/converter
  SwrContext* audio_resampler_{nullptr};
  std::uint64_t audio_resample_key_{0};
  SwsContext* video_scaler_{nullptr};

  struct VideoConvertState;
  std::unique_ptr<VideoConvertState> video_convert_;

  struct SDLAudio;
  std::unique_ptr<SDLAudio> sdl_audio_;

  // Stream parameters
  AudioParams audio_params_{};
  VideoParams video_params_{};

  // Media information
  mutable std::mutex info_mutex_;
  MediaInfo media_info_;
  std::chrono::milliseconds current_position_{0};
  std::chrono::steady_clock::time_point clock_origin_{};
  std::chrono::milliseconds last_emitted_position_{0};

  // Playback state
  mutable std::mutex state_mutex_;
  PlaybackState playback_state_{PlaybackState::Stopped};
  std::atomic<double> playback_rate_{1.0};
  std::atomic<double> volume_{1.0};
  std::atomic<bool> muted_{false};

  // Decoding thread
  std::thread decode_thread_;
  std::atomic<bool> should_stop_decoding_{false};

  // Network stall watchdog, driven by ffmpegInterruptCallback() from inside
  // FFmpeg's IO poll loop: the decode thread timestamps each media read,
  // the callback reports a quiet source (BufferingStarted) and aborts the
  // read only once the quiet window passes the stall tolerance — aborting
  // earlier would kill the connection instead of letting it recover.
  std::atomic<std::chrono::milliseconds::rep> network_read_started_ms_{0};
  std::atomic<bool> network_stall_reported_{false};
  std::atomic<bool> network_stall_exceeded_{false};

  std::condition_variable decode_cv_;
  std::mutex decode_mutex_;

  // Frame queues (thread-safe)
  mutable std::mutex video_queue_mutex_;
  std::queue<DecodedFrame> video_queue_;

  mutable std::mutex audio_queue_mutex_;
  std::queue<DecodedFrame> audio_queue_;

  // Error handling
  mutable std::mutex error_mutex_;
  std::string last_error_;

  // Seek operation
  std::atomic<bool> seek_requested_{false};
  std::atomic<std::chrono::milliseconds::rep> seek_target_{0};

  // Pending audio track switch (guarded by pending_audio_mutex_)
  mutable std::mutex pending_audio_mutex_;
  AVCodecContext* pending_audio_decoder_{nullptr};
  int pending_audio_track_{-1};
};

/**
 * Factory function to create FFmpeg backend instance.
 */
std::unique_ptr<IBackend> makeFFmpegBackend();

} // namespace soar
