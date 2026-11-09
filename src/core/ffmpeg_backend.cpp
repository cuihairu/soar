// FFmpegBackend lifecycle and public API: construction,
// Split out of ffmpeg_backend.cpp; the backend TUs share
// ffmpeg_backend_internal.h.

#include "soar/core/ffmpeg_backend.h"
#include "ffmpeg_backend_internal.h"

namespace soar {


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
  // hwdec_ must be armed before setupDecoders reads it (opened_source_ is
  // only assigned after the whole open path succeeds). Arm it under the
  // same mutex: close() clears hwdec_ under decode_mutex_, so an unlocked
  // assign here races a concurrent close in the open/close storm.
  bool ok = false;
  {
    std::lock_guard<std::mutex> media_lock(decode_mutex_);
    hwdec_ = source.hwdec;

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

    // HDR is a transfer property, read once per open: the decoder's own
    // copy of the stream header (it reads the bitstream VUI) with the
    // container tag as the fallback for streams without one. A stream's
    // color space does not change mid-playback, so there is no per-frame
    // re-check; the per-frame path still repairs an untagged frame from
    // the decoder (runHdrToneMap).
    AVColorTransferCharacteristic trc = AVCOL_TRC_UNSPECIFIED;
    if (video_stream_index_ >= 0 &&
        video_stream_index_ < static_cast<int>(format_ctx_->nb_streams) &&
        format_ctx_->streams[video_stream_index_]->codecpar) {
      trc = format_ctx_->streams[video_stream_index_]->codecpar->color_trc;
    }
    if (video_decoder_ && video_decoder_->color_trc != AVCOL_TRC_UNSPECIFIED) {
      trc = video_decoder_->color_trc;
    }
    hdr_video_ = isHdrTransfer(trc);
    media_info_.hdr_video = hdr_video_;
    hdr_state_.store(hdr_video_ ? HdrToneMapState::Armed : HdrToneMapState::Off,
                     std::memory_order_relaxed);

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
    hwdec_.clear();
    hdr_video_ = false;
    hdr_state_.store(HdrToneMapState::Off, std::memory_order_relaxed);

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
    //
    // Bounded: the rewind runs with should_stop_decoding_ already reset, so
    // the interrupt callback would not abort it, and the stall watchdog is
    // not armed (it only timestamps av_read_frame). A demuxer whose seek
    // re-fetches a manifest could otherwise sit on a dead source for good,
    // holding this lock and the caller's thread with it (BUGS.md #4). A
    // seek that outlives the budget fails instead; play() then rewinds by
    // its own path and the position is already zeroed below.
    bounded_io_deadline_ms_.store(steadyNowMs() + kStopRewindLimit.count());
    (void)seekToTimestamp(std::chrono::milliseconds(0), /*emit_event=*/false);
    bounded_io_deadline_ms_.store(0);
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


AssRenderer* FFmpegBackend::assRenderer() {
  // Stable for the backend's lifetime; AssRenderer locks internally, so
  // the UI may render while the decode thread feeds.
  return ass_renderer_.available() ? &ass_renderer_ : nullptr;
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
// Factory Function
//=============================================================================

std::unique_ptr<IBackend> makeFFmpegBackend() {
  return std::make_unique<FFmpegBackend>();
}

} // namespace soar
