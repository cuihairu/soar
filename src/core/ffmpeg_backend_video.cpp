// FFmpegBackend video render path: sws convert, HDR tone
// Split out of ffmpeg_backend.cpp; the backend TUs share
// ffmpeg_backend_internal.h.

#include "soar/core/ffmpeg_backend.h"
#include "ffmpeg_backend_internal.h"

namespace soar {


bool FFmpegBackend::saveScreenshot(const std::string& path, bool forceFailEncoder) {
  // Snapshot the retained frame first: the encode below does file IO and
  // must not run under video_frame_mutex_, which the decode thread holds
  // while filling frames.
  DecodedVideoFrame frame;
  {
    std::lock_guard<std::mutex> lock(video_frame_mutex_);
    if (presented_video_frame_.width <= 0 || presented_video_frame_.height <= 0) {
      // Nothing presented yet: never played, audio-only media, or a
      // closed backend. Recorded without an event (see the header).
      return fail("screenshot: no video frame has been presented", false);
    }
    frame = presented_video_frame_;
  }

  std::string error;
  const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_PNG);
  // No null-encoder special case: avcodec_alloc_context3 tolerates a null
  // codec, and avcodec_open2 refuses the resulting context below, so a
  // missing PNG encoder still fails cleanly through the open arm.
  AVCodecContext* ctx = avcodec_alloc_context3(encoder);
  AVFrame* yuv = av_frame_alloc();
  AVFrame* rgb = av_frame_alloc();
  AVPacket* packet = av_packet_alloc();
  SwsContext* sws = nullptr;

  if (!ctx || !yuv || !rgb || !packet) {
    error = "screenshot: PNG encoder unavailable or out of memory";
  } else {
    ctx->width = frame.width;
    ctx->height = frame.height;
    ctx->pix_fmt = forceFailEncoder ? AV_PIX_FMT_NONE : AV_PIX_FMT_RGB24;
    ctx->time_base = AVRational{1, 1};
    if (avcodec_open2(ctx, encoder, nullptr) < 0) {
      error = "screenshot: PNG encoder could not be opened";
    }
  }

  // Wrap the retained YUV420P planes (contiguous, stride == width,
  // normalized by renderVideoFrame) in an AVFrame and convert to the
  // RGB24 the PNG encoder takes. The conversion context is per-call:
  // video_scaler_/video_convert_ belong to the decode thread.
  if (error.empty()) {
    yuv->format = AV_PIX_FMT_YUV420P;
    yuv->width = frame.width;
    yuv->height = frame.height;
    yuv->data[0] = frame.y.data();
    yuv->linesize[0] = frame.stride_y;
    yuv->data[1] = frame.u.data();
    yuv->linesize[1] = frame.stride_u;
    yuv->data[2] = frame.v.data();
    yuv->linesize[2] = frame.stride_v;

    rgb->format = AV_PIX_FMT_RGB24;
    rgb->width = frame.width;
    rgb->height = frame.height;
    if (av_frame_get_buffer(rgb, 32) < 0) {
      error = "screenshot: RGB frame allocation failed";
    } else {
      sws = sws_getContext(frame.width, frame.height, AV_PIX_FMT_YUV420P,
                           frame.width, frame.height, AV_PIX_FMT_RGB24,
                           SWS_BILINEAR, nullptr, nullptr, nullptr);
      if (!sws) {
        error = "screenshot: color conversion setup failed";
      } else if (sws_scale(sws, yuv->data, yuv->linesize, 0, frame.height,
                           rgb->data, rgb->linesize) <= 0) {
        error = "screenshot: color conversion failed";
      }
    }
  }

  if (error.empty()) {
    rgb->pts = 0;
    if (avcodec_send_frame(ctx, rgb) < 0) {
      error = "screenshot: PNG encode failed";
    } else {
      // PNG is intra-only: the drained packet(s) are the whole image.
      FILE* out = std::fopen(path.c_str(), "wb");
      if (!out) {
        error = "screenshot: cannot write " + path;
      } else {
        std::size_t expected = 0;
        std::size_t written = 0;
        while (avcodec_receive_packet(ctx, packet) == 0) {
          expected += static_cast<std::size_t>(packet->size);
          written += std::fwrite(packet->data, 1,
                                 static_cast<std::size_t>(packet->size), out);
          av_packet_unref(packet);
        }
        const bool close_ok = std::fclose(out) == 0;
        if (!close_ok || expected == 0 || written != expected) {
          error = "screenshot: write failed for " + path;
        }
      }
    }
  }

  sws_freeContext(sws);
  av_packet_free(&packet);
  av_frame_free(&rgb);
  av_frame_free(&yuv);
  avcodec_free_context(&ctx);
  if (!error.empty()) {
    return fail(std::move(error), false);
  }
  return true;
}


bool FFmpegBackend::grabVideoFrame(std::chrono::milliseconds position,
                                   DecodedVideoFrame& out) {
  out = DecodedVideoFrame{};

  // Snapshot the URI under decode_mutex_ and run the whole grab outside
  // every lock (exportSubtitleText's rule: file/network IO must not block
  // the decode thread). The input belongs to this call alone — the decode
  // thread's own input is untouched.
  std::string uri;
  {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    if (!format_ctx_) {
      return fail("grabVideoFrame: no media opened", /*emit_event=*/false);
    }
    uri = opened_source_.uri;
  }

  AVFormatContext* ctx = nullptr;
  int ret = avformat_open_input(&ctx, uri.c_str(), nullptr, nullptr);
  if (ret < 0) {
    return fail(fmt::format("grabVideoFrame: cannot reopen '{}': {}", uri,
                            avError(ret)),
                /*emit_event=*/false);
  }
  const auto close_ctx = [&]() { avformat_close_input(&ctx); };

  ret = avformat_find_stream_info(ctx, nullptr);
  if (ret < 0) {
    const std::string err =
        fmt::format("grabVideoFrame: no stream info: {}", avError(ret));
    close_ctx();
    return fail(err, /*emit_event=*/false);
  }
  const int vstream =
      av_find_best_stream(ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (vstream < 0) {
    close_ctx();
    return fail("grabVideoFrame: no video stream", /*emit_event=*/false);
  }
  const AVStream* stream = ctx->streams[vstream];
  const AVRational tb = stream->time_base;

  // Software decode only: the caller asks for one small frame on a side
  // thread, and a hwaccel context would need the same device plumbing as
  // playback. The decoder is found explicitly (alloc_context3(nullptr) +
  // open2(nullptr) is refused by libavcodec for some codecs) — the same
  // pattern the subtitle extractor uses.
  const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
  if (codec == nullptr) {
    close_ctx();
    return fail("grabVideoFrame: no decoder for this codec", /*emit_event=*/false);
  }
  AVCodecContext* decoder = avcodec_alloc_context3(codec);
  if (decoder == nullptr) {
    close_ctx();
    return fail("grabVideoFrame: cannot allocate decoder", /*emit_event=*/false);
  }
  ret = avcodec_parameters_to_context(decoder, stream->codecpar);
  if (ret < 0) {
    avcodec_free_context(&decoder);
    close_ctx();
    return fail(
        fmt::format("grabVideoFrame: cannot copy codec params: {}", avError(ret)),
        /*emit_event=*/false);
  }
  ret = avcodec_open2(decoder, codec, nullptr);
  if (ret < 0) {
    avcodec_free_context(&decoder);
    close_ctx();
    return fail(
        fmt::format("grabVideoFrame: cannot open decoder: {}", avError(ret)),
        /*emit_event=*/false);
  }

  // Seek backward to the keyframe at/just before the request, then decode
  // forward to the frame at or past it. A non-seekable source fails here —
  // a hover thumb needs the seek, and reading a live stream forward to an
  // arbitrary offset would block the caller.
  const int64_t target = av_rescale_q(position.count(), AVRational{1, 1000}, tb);
  ret = av_seek_frame(ctx, vstream, target, AVSEEK_FLAG_BACKWARD);
  if (ret < 0) {
    avcodec_free_context(&decoder);
    close_ctx();
    return fail(fmt::format("grabVideoFrame: cannot seek: {}", avError(ret)),
                /*emit_event=*/false);
  }

  AVPacket* packet = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  bool have_frame = false;
  bool reached = false;
  int recv_ret = 0;
  if (packet != nullptr && frame != nullptr) {
    while ((ret = av_read_frame(ctx, packet)) >= 0) {
      if (packet->stream_index != vstream) {
        av_packet_unref(packet);
        continue;
      }
      const int send_ret = avcodec_send_packet(decoder, packet);
      av_packet_unref(packet);
      if (send_ret < 0) {
        continue;
      }
      while ((recv_ret = avcodec_receive_frame(decoder, frame)) == 0) {
        have_frame = true;
        // A frame with no valid timestamp can never be "at or past the
        // request": NOPTS would compare as >= target and wrongly accept it.
        // best_effort_timestamp is the decoded frame's authoritative time;
        // only it is compared, and NOPTS is skipped explicitly.
        const int64_t ts = frame->best_effort_timestamp;
        if (ts != AV_NOPTS_VALUE && ts >= target) {
          reached = true;
          break;
        }
        av_frame_unref(frame);
      }
      if (recv_ret < 0 && recv_ret != AVERROR(EAGAIN) &&
          recv_ret != AVERROR_EOF) {
        av_frame_unref(frame);
        break;  // decode error: stop reading, report below
      }
      if (reached) break;
      av_frame_unref(frame);
    }
    // Drain the decoder's buffered frames after the last packet: without a
    // flush (send_packet(nullptr)), frames held at the tail of a short GOP
    // or near EOF are never received, so a valid grab near the end of the
    // file would report "no decodable frame". Flush and collect any final
    // frame that reaches the target before giving up.
    if (!reached) {
      avcodec_send_packet(decoder, nullptr);
      while ((recv_ret = avcodec_receive_frame(decoder, frame)) == 0) {
        have_frame = true;
        const int64_t ts = frame->best_effort_timestamp;
        if (ts != AV_NOPTS_VALUE && ts >= target) {
          reached = true;
          break;
        }
        av_frame_unref(frame);
      }
      // Keep the frame that reached the target for sws_scale below: the
      // unconditional unref would clear frame->data/linesize to NULL and
      // feed sws_scale a null source (the same guard the main loop has).
      if (!reached) av_frame_unref(frame);
    }
  }

  bool ok = false;
  // Only a frame that actually reached the request is valid input for the
  // scaler. have_frame alone is not enough: it is set on every received
  // frame, but a frame below the target is unref'd before the next read,
  // so a grab that never reaches the target must report "no decodable
  // frame" rather than scaling an unref'd (null-data) AVFrame.
  if (reached) {
    // Scale to the fixed 16:9 thumb, tightly packed YUV420P (stride ==
    // width) so the UI uploads it as one SDL IYUV texture.
    constexpr int kThumbW = 160;
    constexpr int kThumbH = 90;
    SwsContext* sws = sws_getContext(decoder->width, decoder->height,
                                     decoder->pix_fmt, kThumbW, kThumbH,
                                     AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr,
                                     nullptr, nullptr);
    if (sws != nullptr) {
      out.width = kThumbW;
      out.height = kThumbH;
      out.stride_y = kThumbW;
      out.stride_u = kThumbW / 2;
      out.stride_v = kThumbW / 2;
      out.y.resize(static_cast<std::size_t>(kThumbW) * kThumbH);
      out.u.resize(static_cast<std::size_t>(kThumbW / 2) * (kThumbH / 2));
      out.v.resize(static_cast<std::size_t>(kThumbW / 2) * (kThumbH / 2));
      std::uint8_t* dst_data[4] = {out.y.data(), out.u.data(), out.v.data(),
                                   nullptr};
      int dst_lines[4] = {out.stride_y, out.stride_u, out.stride_v, 0};
      sws_scale(sws, frame->data, frame->linesize, 0, decoder->height,
                dst_data, dst_lines);
      out.pts = position;
      ok = true;
      sws_freeContext(sws);
    } else {
      fail("grabVideoFrame: cannot create scaler", /*emit_event=*/false);
    }
  }

  av_frame_free(&frame);
  av_packet_free(&packet);
  avcodec_free_context(&decoder);
  close_ctx();
  if (!ok) {
    fail(have_frame ? "grabVideoFrame: cannot scale frame"
                    : "grabVideoFrame: no decodable frame",
         /*emit_event=*/false);
  }
  return ok;
}


bool FFmpegBackend::buildHdrToneMap(const AVFrame* frame) {
  if (!frame || frame->width <= 0 || frame->height <= 0 ||
      frame->format == AV_PIX_FMT_NONE) {
    return false;
  }

  auto tm = std::make_unique<HdrToneMap>();
  tm->graph = avfilter_graph_alloc();
  if (!tm->graph) {
    return false;
  }

  // zscale is the only correct recipe (see the file-local comment above);
  // a build without libzimg gets PassThrough instead of a wrong picture.
  if (!avfilter_get_by_name("zscale")) {
    return false;
  }

  // The input node takes the decoded frame's own shape; the recipe ends in
  // format=yuv420p, so the caller treats the output like the plain convert
  // path's output. A missing aspect falls back to square pixels (the buffer
  // filter refuses 0/1). No color properties are passed: which ones this
  // build's buffer filter accepts differs by version, and the pushed frame
  // already carries its own tags (runHdrToneMap restores the transfer tag
  // from the decoder when a frame arrives untagged).
  AVRational tb{1, 1};
  if (format_ctx_ && video_stream_index_ >= 0 &&
      video_stream_index_ < static_cast<int>(format_ctx_->nb_streams) &&
      format_ctx_->streams[video_stream_index_]->time_base.den > 0) {
    tb = format_ctx_->streams[video_stream_index_]->time_base;
  }
  const AVRational sar = frame->sample_aspect_ratio.num > 0 &&
                                 frame->sample_aspect_ratio.den > 0
                             ? frame->sample_aspect_ratio
                             : AVRational{1, 1};
  const std::string input_args = fmt::format(
      "video_size={}x{}:pix_fmt={}:time_base={}/{}:pixel_aspect={}/{}",
      frame->width, frame->height, static_cast<int>(frame->format),
      tb.num, tb.den, sar.num, sar.den);

  const AVFilter* in_filter = avfilter_get_by_name("buffer");
  const AVFilter* out_filter = avfilter_get_by_name("buffersink");
  if (!in_filter || !out_filter ||
      avfilter_graph_create_filter(&tm->src, in_filter, "in", input_args.c_str(),
                                   nullptr, tm->graph) < 0 ||
      avfilter_graph_create_filter(&tm->sink, out_filter, "out", nullptr,
                                   nullptr, tm->graph) < 0) {
    return false; // tm owns the graph; its destructor releases everything
  }

  AVFilterInOut* outputs = avfilter_inout_alloc();
  AVFilterInOut* inputs = avfilter_inout_alloc();
  int ret = -1;
  if (outputs && inputs) {
    outputs->name = av_strdup("in");
    outputs->filter_ctx = tm->src;
    outputs->pad_idx = 0;
    inputs->name = av_strdup("out");
    inputs->filter_ctx = tm->sink;
    inputs->pad_idx = 0;
    ret = avfilter_graph_parse_ptr(tm->graph, kHdrToneMapZscale, &inputs,
                                   &outputs, nullptr);
    if (ret >= 0) {
      ret = avfilter_graph_config(tm->graph, nullptr);
    }
  }
  avfilter_inout_free(&inputs);
  avfilter_inout_free(&outputs);
  if (ret < 0) {
    return false;
  }

  hdr_tone_map_ = std::move(tm);
  hdr_state_.store(HdrToneMapState::Zscale, std::memory_order_relaxed);
  return true;
}


bool FFmpegBackend::runHdrToneMap(const AVFrame* frame, AVFrame** out) {
  HdrToneMap* tm = hdr_tone_map_.get();
  if (!tm || !frame) {
    return false;
  }

  // A hardware-download or some demux paths hand over frames without the
  // transfer tag; restore it from the decoder so the graph applies the
  // right EOTF instead of treating the HDR code values as SDR gamma.
  AVFrame* video = const_cast<AVFrame*>(frame);
  if (video->color_trc == AVCOL_TRC_UNSPECIFIED && video_decoder_ &&
      video_decoder_->color_trc != AVCOL_TRC_UNSPECIFIED) {
    video->color_trc = video_decoder_->color_trc;
  }

  // KEEP_REF: the caller keeps owning the decoded frame (processVideoFrame
  // frees it after renderVideoFrame returns).
  if (av_buffersrc_add_frame_flags(tm->src, video, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
    return false;
  }

  // The sink fills a caller-owned frame (6.x through 9.x alike).
  AVFrame* mapped = av_frame_alloc();
  if (!mapped) {
    return false;
  }
  const int ret = av_buffersink_get_frame(tm->sink, mapped);
  if (ret == AVERROR(EAGAIN)) {
    // Nothing came back for this push: pass the frame through the plain
    // convert path. Every recipe is a 1:1 chain with no internal queueing
    // (no frame threading is enabled on the graph), so this cannot leave a
    // stale frame behind for the next pull.
    av_frame_free(&mapped);
    return false;
  }
  if (ret < 0) {
    av_frame_free(&mapped);
    return false;
  }
  *out = mapped;
  return true;
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

  // HDR sources: convert the frame to SDR before anything downstream sees
  // it. The graph is built once on the first HDR frame; if it cannot be
  // built the source stays on PassThrough (the plain convert path, like
  // the hwdec's silent software fallback) instead of failing playback.
  AVFrame* tone_mapped = nullptr;
  if (hdr_video_) {
    if (!hdr_tone_map_ && !buildHdrToneMap(frame)) {
      hdr_state_.store(HdrToneMapState::PassThrough, std::memory_order_relaxed);
    }
    if (hdr_tone_map_) {
      AVFrame* mapped = nullptr;
      if (runHdrToneMap(frame, &mapped)) {
        tone_mapped = mapped;
      }
    }
  }

  // A tone-mapped frame is yuv420p by construction, so the swscale block
  // below is skipped exactly when a graph frame is live.
  const AVFrame* src = tone_mapped ? tone_mapped : frame;
  const auto src_fmt =
      tone_mapped ? AV_PIX_FMT_YUV420P : static_cast<AVPixelFormat>(frame->format);

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

  // Last use of the tone-mapped frame was the copies above; release it
  // before the mailbox swap.
  if (tone_mapped) {
    av_frame_free(&tone_mapped);
  }

  {
    std::lock_guard<std::mutex> lock(video_frame_mutex_);
    std::swap(latest_video_frame_, staging_video_frame_);
    video_frame_ready_ = true;
    // Retained copy for saveScreenshot(); the mailbox above is handed to
    // the app by swap, so nothing reliable survives there.
    presented_video_frame_ = latest_video_frame_;
  }
}

} // namespace soar
