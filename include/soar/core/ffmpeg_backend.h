#pragma once

#include "soar/core/backend.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

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
  bool openContext(const std::string& uri);
  void closeContext();
  bool findStreamInfo();
  bool setupDecoders();
  void cleanupDecoders();

  // Decoding
  void decodeLoop();
  bool decodeVideoFrame(AVFrame* frame);
  bool decodeAudioFrame(AVFrame* frame);
  void queueVideoFrame(AVFrame* frame, std::chrono::milliseconds pts);
  void queueAudioFrame(AVFrame* frame, std::chrono::milliseconds pts);

  // Frame processing
  void processVideoFrame(const DecodedFrame& frame);
  void processAudioFrame(const DecodedFrame& frame);

  // Rendering (to be implemented)
  void renderVideoFrame(const AVFrame* frame);
  void playAudioFrame(const AVFrame* frame);

  // Seeking
  bool flushDecoders();
  bool seekToTimestamp(std::chrono::milliseconds position);

  // Utility functions
  static std::string getCodecName(AVCodecContext* ctx);
  static std::chrono::milliseconds fromAVTimestamp(int64_t pts, int time_base_num, int time_base_den);
  static int64_t toAVTimestamp(std::chrono::milliseconds ms, AVRational time_base);
  std::string avError(int errnum);

  // Event emission
  void emit(Event e);
  bool fail(std::string message);

  // Member variables

  // Event system
  mutable std::mutex event_mutex_;
  IEventSink* event_sink_{nullptr};

  // FFmpeg contexts
  AVFormatContext* format_ctx_{nullptr};
  AVCodecContext* video_decoder_{nullptr};
  AVCodecContext* audio_decoder_{nullptr};

  int video_stream_index_{-1};
  int audio_stream_index_{-1};

  // Rescalers/converter
  SwrContext* audio_resampler_{nullptr};
  SwsContext* video_scaler_{nullptr};

  // Stream parameters
  AudioParams audio_params_{};
  VideoParams video_params_{};

  // Media information
  mutable std::mutex info_mutex_;
  MediaInfo media_info_;
  std::chrono::milliseconds current_position_{0};
  std::chrono::milliseconds start_time_{0};

  // Playback state
  mutable std::mutex state_mutex_;
  PlaybackState playback_state_{PlaybackState::Stopped};
  std::atomic<double> playback_rate_{1.0};
  std::atomic<double> volume_{1.0};
  std::atomic<bool> muted_{false};

  // Decoding thread
  std::thread decode_thread_;
  std::atomic<bool> should_stop_decoding_{false};
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
};

/**
 * Factory function to create FFmpeg backend instance.
 */
std::unique_ptr<IBackend> makeFFmpegBackend();

} // namespace soar
