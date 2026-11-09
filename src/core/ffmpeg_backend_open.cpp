// FFmpegBackend container open, stream/decoder setup,
// Split out of ffmpeg_backend.cpp; the backend TUs share
// ffmpeg_backend_internal.h.

#include "soar/core/ffmpeg_backend.h"
#include "ffmpeg_backend_internal.h"

namespace soar {


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
  // Bounded avio outside a watched media read: the stall watchdog below is
  // armed only around av_read_frame, so a seek that makes FFmpeg re-fetch a
  // playlist would otherwise wait on a dead source for good. Checked before
  // the started_ms early-out, since such a read carries no timestamp.
  const auto deadline_ms = self->bounded_io_deadline_ms_.load();
  if (deadline_ms != 0 && steadyNowMs() >= deadline_ms) {
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
    // A demuxer can refuse a container it finished parsing: FFmpeg 9's
    // timecode handling lists a self-referencing tmcd track in its stream
    // group twice and fails the whole open with EEXIST, while an 8.x build
    // opens the same file and the stream scan below calls it stream-less.
    // Nothing playable travels either way, so the verdict must not depend
    // on the build's failure point.
    if (ret == AVERROR(EEXIST)) {
      return fatal(fmt::format("open: '{}': {}", source.uri, kNoStreamsVerdict),
                   /*emit_event=*/false);
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
    return fatal(fmt::format("findStreamInfo: {}", kNoStreamsVerdict),
                 /*emit_event=*/false);
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

    // Hardware decode (docs/mvp.md §3): when the source asks for it, try
    // to arm the decoder with a hwaccel device context. Every failure —
    // codec without hwaccel support, device that won't open, open2 that
    // rejects the hwaccel — falls through to the all-software decoder
    // below; hwaccel is an optimization, never a requirement.
    // CI and other GPU-less machines cannot open a hwaccel device, which
    // would leave everything downstream of the arm (get_format
    // negotiation, the open2 fallback, frame download, teardown)
    // unobservable. SOAR_TEST_HWACCEL_ARM (any non-empty value) stands in
    // for the codec config lookup and the device create with a plain
    // ref-counted buffer (the saveScreenshot forceFailEncoder precedent):
    // the real libavcodec machinery still runs everything downstream.
    // Production runs never see the variable.
    const char* arm_env = std::getenv("SOAR_TEST_HWACCEL_ARM");
    if (!hwdec_.empty() && hwdec_ != "none") {
      for (const AVHWDeviceType hw_type : hwdecDevices(hwdec_)) {
        enum AVPixelFormat hw_pix_fmt = AV_PIX_FMT_NONE;
        if (arm_env && *arm_env) {
          // The stream's encoded pixel format steers get_format toward what
          // the decoder will output; the post-open alignment below makes
          // the match exact whatever the decoder negotiates.
          const int fmt = codecpar->format;
          if (fmt >= 0) {
            hw_pix_fmt = static_cast<enum AVPixelFormat>(fmt);
          }
        } else {
          for (int i = 0;; ++i) {
            const AVCodecHWConfig* config = avcodec_get_hw_config(codec, i);
            if (!config) {
              break;
            }
            if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
                config->device_type == hw_type) {
              hw_pix_fmt = config->pix_fmt;
              break;
            }
          }
        }
        if (hw_pix_fmt == AV_PIX_FMT_NONE) {
          continue;  // codec has no hwaccel for this device
        }

        AVBufferRef* device_ctx = nullptr;
        if (arm_env && *arm_env) {
          // Never dereferenced as a device by the paths the test drives: a
          // software pixel format negotiated below keeps libavcodec off the
          // hwaccel init that would read it.
          device_ctx = av_buffer_alloc(1);
        } else {
          const int create_ret =
              av_hwdevice_ctx_create(&device_ctx, hw_type, nullptr, nullptr, 0);
          if (create_ret < 0) {
            continue;  // no such device on this system
          }
        }
        if (!device_ctx) continue;  // device allocation failed

        // The decoder takes its OWN reference (the codec context is freed
        // by avcodec_free_context in cleanupDecoders); hw_device_ctx_ keeps
        // this instance's reference until the same teardown — mirroring
        // doc/examples/hw_decode.c. Assignment without the extra ref would
        // hand the codec our only one and double-unref it at teardown.
        AVBufferRef* codec_ref = av_buffer_ref(device_ctx);
        if (!codec_ref) { av_buffer_unref(&device_ctx); continue; }
        hw_pix_fmt_ = static_cast<int>(hw_pix_fmt);
        video_decoder_->opaque = &hw_pix_fmt_;
        video_decoder_->get_format = &hwGetFormat;
        video_decoder_->hw_device_ctx = codec_ref;
        hw_device_ctx_ = device_ctx;
        break;  // armed — stop trying further devices
      }
    }

    ret = avcodec_open2(video_decoder_, codec, nullptr);
    if (ret >= 0 && arm_env && *arm_env) {
      // Test-only alignment: the decoder's actual output format is the only
      // format its frames can arrive in — sync the download-branch
      // comparison to it (the pre-open stream format only steered
      // get_format).
      hw_pix_fmt_ = video_decoder_->pix_fmt;
    }
    if (ret < 0 && hw_device_ctx_) {
      // The hwaccel armed but the decoder rejected it at open (driver
      // mismatch, unsupported profile...). docs/mvp.md §3 makes hwaccel an
      // optimization, never a requirement: drop it and open the plain
      // software decoder instead of failing the whole source.
      disarmHwAccel();
      ret = avcodec_open2(video_decoder_, codec, nullptr);
    }
    if (ret < 0) {
      // A hwaccel that armed but failed to open needs no dedicated release
      // here: the caller's failure branch runs cleanupDecoders(), which
      // frees the video decoder (dropping the codec context's reference)
      // and then this instance's device reference.
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
  // Hardware decode teardown first, while the video decoder (which holds
  // the codec's own device reference) is still available to disarmHwAccel.
  disarmHwAccel();

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

  // The HDR tone-map graph belongs to the decode thread like the convert
  // state above; the decode thread is stopped whenever this runs.
  hdr_tone_map_.reset();

}


// Drop a live hwaccel arm: release both references to the device context
// (the video decoder's own and this instance's) and reset the negotiation
// state, so the decoder can be re-opened as plain software (the open2
// fallback in setupDecoders) or freed (cleanupDecoders) without a dangling
// device reference. Safe both with the video decoder still alive and after
// avcodec_free_context.
void FFmpegBackend::disarmHwAccel() {
  if (video_decoder_ && video_decoder_->hw_device_ctx) {
    // The codec context holds its own reference; drop it through the field
    // so neither the open2 retry nor avcodec_free_context sees it again.
    av_buffer_unref(&video_decoder_->hw_device_ctx);
  }
  if (hw_device_ctx_) {
    av_buffer_unref(&hw_device_ctx_);
    hw_device_ctx_ = nullptr;
  }
  hw_pix_fmt_ = -1;
  if (video_decoder_) {
    video_decoder_->opaque = nullptr;
    video_decoder_->get_format = nullptr;
  }
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

} // namespace soar
