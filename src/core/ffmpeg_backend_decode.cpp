// FFmpegBackend decode loop, frame queues and the
// Split out of ffmpeg_backend.cpp; the backend TUs share
// ffmpeg_backend_internal.h.

#include "soar/core/ffmpeg_backend.h"
#include "ffmpeg_backend_internal.h"

namespace soar {


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

        if (hw_pix_fmt_ >= 0 && frame->format == hw_pix_fmt_) {
          // Hardware frame: download to software YUV420P so the render path
          // (queue, convert, present) stays the all-software one. The
          // download target is the format the render path converts from
          // natively; a transfer failure drops the frame rather than
          // feeding the converter a format sws_scale cannot read.
          AVFrame* sw_frame = av_frame_alloc();
          if (sw_frame) {
            sw_frame->format = AV_PIX_FMT_YUV420P;
            sw_frame->width = frame->width;
            sw_frame->height = frame->height;
            if (av_hwframe_transfer_data(sw_frame, frame, 0) >= 0) {
              sw_frame->pts = frame->pts;
              sw_frame->best_effort_timestamp = frame->best_effort_timestamp;
              queueVideoFrame(sw_frame, pts);
            }
            av_frame_free(&sw_frame);
          }
          av_frame_unref(frame);
        } else {
          queueVideoFrame(frame, pts);
          av_frame_unref(frame);
        }
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

} // namespace soar
