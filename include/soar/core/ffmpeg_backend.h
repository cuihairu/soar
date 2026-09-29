#pragma once

#include "soar/core/backend.h"
#include "soar/core/subtitle_text.h"

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
struct AVSubtitle;
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

/**
 * Decoded video frame (YUV420P) for UI rendering.
 * Owned by the backend; the UI pulls frames via tryGetVideoFrame().
 */
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

/**
 * Decoded subtitle frame for UI rendering.
 * Owned by the backend; the UI pulls frames via tryGetSubtitleFrame().
 */
struct DecodedSubtitleFrame {
  std::string text;
  std::chrono::milliseconds pts{0};
  std::chrono::milliseconds duration{0};
};

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
  bool loadExternalSubtitle(const std::string& path, TrackId& out_id) override;

  // Style-faithful ASS rendering (docs/mvp.md §6): non-null when the build
  // links libass; the embedded ASS/SSA track selected at open feeds it and
  // the UI renders it at frame-present time (the plain-text overlay stays
  // authoritative whenever this is null).
  AssRenderer* assRenderer() override;

  // True while the libass canvas holds a whole document (an external
  // .ass/.ssa script or a synthesized text-sidecar one, batch 1c): the
  // plain-text drawing path must stay quiet for the same selection or
  // every line would render twice. Always false without libass.
  bool subtitleDocumentActive() const {
    return ass_document_active_.load(std::memory_order_relaxed);
  }

  bool setLoopAB(std::chrono::milliseconds a, std::chrono::milliseconds b) override;
  bool clearLoopAB() override;
  bool loopAB(std::chrono::milliseconds& out_a, std::chrono::milliseconds& out_b) const override;

  std::vector<std::string> audioOutputDevices() const override;
  std::string currentAudioOutputDevice() const override;
  bool selectAudioOutputDevice(const std::string& name) override;

  PlaybackState state() const override;
  std::string lastError() const override;

  // Optional: allow the app to pull the latest decoded video frame (YUV420P).
  // Thread-safe; returns true only when a new frame is available.
  bool tryGetVideoFrame(DecodedVideoFrame& out);

  // Optional: allow the app to pull the latest decoded subtitle frame.
  // Thread-safe; returns true only when a new frame is available. The pull
  // also drives external subtitles: a sidecar has no decoder thread to
  // queue frames, so the playhead is advanced here (see pumpExternalCues).
  bool tryGetSubtitleFrame(DecodedSubtitleFrame& out);

  // v0.2 screenshot: encode the most recently presented video frame as a
  // PNG at `path`. Thread-safe (meant for the UI thread). On failure the
  // reason is recorded in lastError() only — no event, no state change:
  // a screenshot is a UI convenience, not a playback error.
  // `forceFailEncoder` is test-only: when true, the codec context is
  // configured with an invalid pixel format so avcodec_open2 fails,
  // exercising the open-failure arm that would otherwise be structurally
  // unreachable (the PNG encoder ships with FFmpeg on every platform).
  bool saveScreenshot(const std::string& path, bool forceFailEncoder = false);

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
  void processSubtitleFrame(const AVSubtitle& sub, std::chrono::milliseconds pts, std::chrono::milliseconds duration);
  bool waitForPresentationTime(std::chrono::milliseconds pts);

  // External subtitles (docs/mvp.md §6, Stage 2). A sidecar file is parsed
  // once at load time into plain cues; there is no decoder and no decode
  // thread behind it, so the frames are pumped from the playhead when the
  // UI pulls them, through the same queue the embedded path uses (the UI
  // cannot tell the two apart). Guarded by external_mutex_, which is never
  // held across a call into the frame queue.
  struct ExternalSubtitle {
    std::string path;
    std::vector<SubtitleCue> cues;
    // Whole ASS/SSA document, when the sidecar is one (empty otherwise).
    // With libass the document renders style-faithfully through
    // AssRenderer::loadDocument once its track is selected; `cues` still
    // carries the plain-text extraction for the no-libass degrade and for
    // frame-count bookkeeping.
    std::string document;
  };
  mutable std::mutex external_mutex_;
  std::vector<ExternalSubtitle> external_subtitles_;
  // Index into external_subtitles_ of the track selectTrack() last selected,
  // or -1 when an embedded track (or nothing) is active.
  int active_external_{-1};
  // How far into the active cue list the pump has got, plus the playhead it
  // last saw. A playhead that went backwards (seek, A-B wrap, replay) re-arms
  // the cursor by rescanning from the top.
  std::size_t external_cue_pos_{0};
  std::chrono::milliseconds external_last_pos_{0};
  // Queue every cue the playhead has reached, newest ones only.
  void pumpExternalCues(std::chrono::milliseconds now);
  // Drop the loaded sidecars (open()/close()).
  void clearExternalSubtitles();

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

  // A-B loop wrap (decode thread only): once the play clock reaches the
  // armed B point, seek back to A and rebase the clock. Returns true when
  // a wrap happened. atEof skips the elapsed>=B check — at end-of-stream
  // any armed loop wraps regardless of where B sits (b == duration is the
  // end-anchored loop).
  bool checkLoopWrap(bool at_eof);

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
  // Last presented frame, retained for saveScreenshot(): the UI mailbox
  // hands frames out by swap, so latest_video_frame_ holds nothing usable
  // once the app has pulled. Filled under video_frame_mutex_.
  DecodedVideoFrame presented_video_frame_{};

  // Subtitle frame handoff (for UI rendering on main thread)
  mutable std::mutex subtitle_frame_mutex_;
  // Small FIFO, not a single-slot mailbox: adjacent SRT cues decode
  // back-to-back whenever decoding outpaces playback, and one slot would
  // silently drop everything but the newest.
  std::queue<DecodedSubtitleFrame> subtitle_frames_;

  // FFmpeg contexts
  AVFormatContext* format_ctx_{nullptr};
  AVCodecContext* video_decoder_{nullptr};
  AVCodecContext* audio_decoder_{nullptr};
  AVCodecContext* subtitle_decoder_{nullptr};

  // Embedded ASS/SSA style renderer (docs/mvp.md §6). Always constructed:
  // without libass it is the inert stub (available() false). One renderer
  // serves every subtitle source the canvas can draw — an embedded stream
  // (fed event lines from the decoder), an external sidecar document
  // (loadDocument), and an external text sidecar, resynthesized into a
  // default-styled document at load time (batch 1c) — so it follows
  // "the last selected subtitle source": setupDecoders arms the embedded
  // feed, selectTrack redirects it to a document track and back.
  // ass_feed_active_ is written from the UI thread (selectTrack,
  // setupDecoders/cleanupDecoders during open/close) and read on the
  // decode thread (processSubtitleFrame), hence the atomic; AssRenderer
  // itself locks. ass_codec_private_ is the embedded stream's codec
  // private blob stashed at open time so re-selecting the embedded track
  // after a document can re-arm the feed without re-demuxing (read under
  // decode_mutex_, which setupDecoders already holds). ass_document_active_
  // records that the renderer currently holds a document track: the
  // decode thread drops the embedded stream's events while it stands (a
  // plain-text echo under the document would draw every line twice), and
  // the UI skips its plain-text drawing for as long as it stands (the
  // canvas owns the selected subtitle — synthesized text sidecars
  // included — and the queue's copies of the same lines must not draw a
  // second time; subtitleDocumentActive() is that query). Atomic like
  // ass_feed_active_, written by selectTrack, re-baselined by every
  // open/close.
  AssRenderer ass_renderer_;
  std::atomic<bool> ass_feed_active_{false};
  std::string ass_codec_private_;
  std::atomic<bool> ass_document_active_{false};

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

  // Requested output endpoint, "" = system default (backend.h contract).
  // Guarded by its own mutex: selected from the UI thread, read by the
  // decode thread when (re)opening the device. `sdl_audio_` itself is
  // only touched under decode control except for a live device switch,
  // which goes through SDLAudio's internal lock.
  mutable std::mutex audio_device_mutex_;
  std::string audio_device_;

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
  // A-B loop endpoints in ms, -1 = disarmed (backend.h contract). Atomics:
  // armed from the caller thread, consumed on the decode thread.
  std::atomic<std::chrono::milliseconds::rep> loop_a_ms_{-1};
  std::atomic<std::chrono::milliseconds::rep> loop_b_ms_{-1};
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
