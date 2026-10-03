// Audio extraction implementation: libavformat to open/decode, libswresample
// to fold any source layout/format/rate down to 16 kHz mono s16le, and the
// libavformat wav muxer to frame it. No backend involvement — this is the
// standalone puller half of the §6 ASR slice.

#include "soar/core/audio_extract.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace soar {
namespace {

std::string avErr(int code) {
  char buf[128] = {0};
  av_strerror(code, buf, sizeof(buf));
  return buf;
}

// RAII wrappers so every early return frees in reverse order.
struct AvFormatDeleter {
  void operator()(AVFormatContext* p) const {
    if (p) avformat_close_input(&p);
  }
};
struct CodecCtxDeleter {
  void operator()(AVCodecContext* c) const {
    if (c) avcodec_free_context(&c);
  }
};
struct SwrDeleter {
  void operator()(SwrContext* s) const {
    if (s) swr_free(&s);
  }
};
struct OutFormatDeleter {
  void operator()(AVFormatContext* p) const {
    if (!p) return;
    if (p->pb) avio_closep(&p->pb);
    avformat_free_context(p);
  }
};

}  // namespace

bool extractAudioToWav(const std::string& uri, const std::string& out_wav,
                       std::string* err) {
  auto fail = [&](const std::string& msg) -> bool {
    if (err) *err = msg;
    return false;
  };

  AVFormatContext* raw_in = nullptr;
  int ret = avformat_open_input(&raw_in, uri.c_str(), nullptr, nullptr);
  if (ret < 0) return fail("openInput: " + avErr(ret));
  std::unique_ptr<AVFormatContext, AvFormatDeleter> in(raw_in);

  ret = avformat_find_stream_info(in.get(), nullptr);
  if (ret < 0) return fail("findStreamInfo: " + avErr(ret));

  int audio_idx = av_find_best_stream(in.get(), AVMEDIA_TYPE_AUDIO, -1, -1,
                                      nullptr, 0);
  if (audio_idx < 0) return fail("no audio track in source");

  AVStream* ast = in->streams[audio_idx];
  const AVCodec* dec = avcodec_find_decoder(ast->codecpar->codec_id);
  if (!dec) return fail("no decoder for audio codec");
  std::unique_ptr<AVCodecContext, CodecCtxDeleter> dec_ctx(
      avcodec_alloc_context3(dec));
  if (!dec_ctx) return fail("alloc decoder ctx");
  avcodec_parameters_to_context(dec_ctx.get(), ast->codecpar);
  ret = avcodec_open2(dec_ctx.get(), dec, nullptr);
  if (ret < 0) return fail("open decoder: " + avErr(ret));

  // Resampler: source layout/format/rate -> 16 kHz mono s16le.
  AVChannelLayout in_layout;
  av_channel_layout_default(&in_layout, dec_ctx->ch_layout.nb_channels);
  AVChannelLayout out_layout;
  av_channel_layout_default(&out_layout, 1);
  SwrContext* raw_swr = nullptr;
  ret = swr_alloc_set_opts2(&raw_swr, &out_layout, AV_SAMPLE_FMT_S16, 16000,
                            &in_layout, dec_ctx->sample_fmt,
                            dec_ctx->sample_rate, 0, nullptr);
  av_channel_layout_uninit(&in_layout);
  av_channel_layout_uninit(&out_layout);
  if (ret < 0) return fail("swr_alloc_set_opts2: " + avErr(ret));
  std::unique_ptr<SwrContext, SwrDeleter> swr(raw_swr);
  ret = swr_init(swr.get());
  if (ret < 0) return fail("swr_init: " + avErr(ret));

  // Output: wav muxer, one pcm_s16le 16 kHz mono stream.
  AVFormatContext* raw_out = nullptr;
  ret = avformat_alloc_output_context2(&raw_out, nullptr, "wav",
                                        out_wav.c_str());
  if (ret < 0 || !raw_out) return fail("alloc output ctx: " + avErr(ret));
  std::unique_ptr<AVFormatContext, OutFormatDeleter> out(raw_out);

  AVStream* ost = avformat_new_stream(out.get(), nullptr);
  if (!ost) return fail("new output stream");
  ost->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
  ost->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
  ost->codecpar->format = AV_SAMPLE_FMT_S16;
  ost->codecpar->sample_rate = 16000;
  av_channel_layout_default(&ost->codecpar->ch_layout, 1);
  ost->codecpar->bit_rate = 16000 * 1 * 16;
  ost->time_base = AVRational{1, 16000};

  ret = avio_open(&out->pb, out_wav.c_str(), AVIO_FLAG_WRITE);
  if (ret < 0) return fail("open output: " + avErr(ret));
  ret = avformat_write_header(out.get(), nullptr);
  if (ret < 0) return fail("write_header: " + avErr(ret));

  int64_t out_pts = 0;
  AVPacket* pkt = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();

  while ((ret = av_read_frame(in.get(), pkt)) >= 0) {
    if (pkt->stream_index != audio_idx) {
      av_packet_unref(pkt);
      continue;
    }
    ret = avcodec_send_packet(dec_ctx.get(), pkt);
    av_packet_unref(pkt);
    if (ret < 0) return fail("send_packet: " + avErr(ret));
    while ((ret = avcodec_receive_frame(dec_ctx.get(), frame)) == 0) {
      const int in_rate = dec_ctx->sample_rate;
      int max_out = frame->nb_samples * 16000 / in_rate + 256;
      std::string buf(static_cast<size_t>(max_out) * 2, '\0');
      uint8_t* out_ptr = reinterpret_cast<uint8_t*>(buf.data());
      // Frame data as plain `const uint8_t**`: swr_convert's `in` takes
      // exactly that on the FFmpeg 6.x line (CI's apt build), and the
      // newer headers taking `const uint8_t* const*` accept it as a
      // qualification conversion. The const* const* form only compiles on
      // the new headers — 6.x rejects it for dropping a const.
      const uint8_t** in_data = const_cast<const uint8_t**>(frame->data);
      int converted = swr_convert(swr.get(), &out_ptr, max_out, in_data,
                                  frame->nb_samples);
      av_frame_unref(frame);
      if (converted < 0) return fail("swr_convert: " + avErr(converted));
      if (converted == 0) continue;
      buf.resize(static_cast<size_t>(converted) * 2);
      AVPacket* opkt = av_packet_alloc();
      av_new_packet(opkt, static_cast<int>(buf.size()));
      std::memcpy(opkt->data, buf.data(), buf.size());
      opkt->pts = opkt->dts = out_pts;
      opkt->duration = converted;
      opkt->stream_index = 0;
      out_pts += converted;
      ret = av_interleaved_write_frame(out.get(), opkt);
      av_packet_free(&opkt);
      if (ret < 0) return fail("write: " + avErr(ret));
    }
    if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
      return fail("receive_frame: " + avErr(ret));
  }

  // Flush decoder.
  avcodec_send_packet(dec_ctx.get(), nullptr);
  while ((ret = avcodec_receive_frame(dec_ctx.get(), frame)) == 0) {
    const int in_rate = dec_ctx->sample_rate;
    int max_out = frame->nb_samples * 16000 / in_rate + 256;
    std::string buf(static_cast<size_t>(max_out) * 2, '\0');
    uint8_t* out_ptr = reinterpret_cast<uint8_t*>(buf.data());
    // Same `const uint8_t**` form as the loop above (FFmpeg 6.x signature).
    const uint8_t** in_data = const_cast<const uint8_t**>(frame->data);
    int converted =
        swr_convert(swr.get(), &out_ptr, max_out, in_data, frame->nb_samples);
    av_frame_unref(frame);
    if (converted < 0) return fail("swr_convert(flush): " + avErr(converted));
    if (converted > 0) {
      buf.resize(static_cast<size_t>(converted) * 2);
      AVPacket* opkt = av_packet_alloc();
      av_new_packet(opkt, static_cast<int>(buf.size()));
      std::memcpy(opkt->data, buf.data(), buf.size());
      opkt->pts = opkt->dts = out_pts;
      opkt->duration = converted;
      opkt->stream_index = 0;
      out_pts += converted;
      ret = av_interleaved_write_frame(out.get(), opkt);
      av_packet_free(&opkt);
      if (ret < 0) return fail("write(flush): " + avErr(ret));
    }
  }

  // Drain resampler tail.
  for (;;) {
    int max_out = static_cast<int>(swr_get_delay(swr.get(), dec_ctx->sample_rate)) + 256;
    if (max_out <= 0) break;
    std::string buf(static_cast<size_t>(max_out) * 2, '\0');
    uint8_t* out_ptr = reinterpret_cast<uint8_t*>(buf.data());
    int converted = swr_convert(swr.get(), &out_ptr, max_out, nullptr, 0);
    if (converted < 0) return fail("swr_convert(drain): " + avErr(converted));
    if (converted == 0) break;
    buf.resize(static_cast<size_t>(converted) * 2);
    AVPacket* opkt = av_packet_alloc();
    av_new_packet(opkt, static_cast<int>(buf.size()));
    std::memcpy(opkt->data, buf.data(), buf.size());
    opkt->pts = opkt->dts = out_pts;
    opkt->duration = converted;
    opkt->stream_index = 0;
    out_pts += converted;
    ret = av_interleaved_write_frame(out.get(), opkt);
    av_packet_free(&opkt);
    if (ret < 0) return fail("write(drain): " + avErr(ret));
  }

  av_write_trailer(out.get());
  av_packet_free(&pkt);
  av_frame_free(&frame);
  return true;
}

}  // namespace soar
