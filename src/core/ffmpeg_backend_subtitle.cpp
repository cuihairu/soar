// FFmpegBackend track selection and external/imported
// Split out of ffmpeg_backend.cpp; the backend TUs share
// ffmpeg_backend_internal.h.

#include "soar/core/ffmpeg_backend.h"
#include "ffmpeg_backend_internal.h"

namespace soar {


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

  // Subtitle selection is decode semantics, not metadata (docs/mvp.md §6):
  // selecting an embedded stream arms the decode gate on it and swaps in a
  // fresh decoder at a packet boundary below; selecting an external track
  // (or the built-in default path before any selection) decides what the
  // gate feeds. Safe in any state: the handover follows the audio switch's
  // pending-decoder contract, so failure leaves the old track playing.
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

} // namespace soar
