#include "transcode_server.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

#ifdef SOAR_WITH_FFMPEG
extern "C" {
#  include <libavcodec/avcodec.h>
#  include <libavformat/avformat.h>
#  include <libavutil/audio_fifo.h>
#  include <libavutil/opt.h>
#  include <libswresample/swresample.h>
#  include <libswscale/swscale.h>
}
#endif

namespace soar::cast {

namespace {

#ifdef _WIN32
using socklen_t = int;

void sockInit() {
  static const bool ready = [] {
    WSADATA d;
    return WSAStartup(MAKEWORD(2, 2), &d) == 0;
  }();
  (void)ready;
}
void sockClose(int s) { closesocket(s); }
int lastNetError() { return WSAGetLastError(); }
int sockRecv(int s, char* p, int n) { return recv(s, p, n, 0); }
int sockSend(int s, const char* p, int n) { return send(s, p, n, 0); }
#else
void sockInit() {}
void sockClose(int s) { ::close(s); }
int lastNetError() { return errno; }
int sockRecv(int s, char* p, int n) {
  return static_cast<int>(::recv(s, p, static_cast<size_t>(n), 0));
}
int sockSend(int s, const char* p, int n) {
  return static_cast<int>(::send(s, p, static_cast<size_t>(n), 0));
}
#endif

bool waitReadable(int s, int ms) {
#ifdef _WIN32
  fd_set set;
  FD_ZERO(&set);
  FD_SET(static_cast<SOCKET>(s), &set);
  timeval tv{};
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  return ::select(s + 1, &set, nullptr, nullptr, &tv) > 0;
#else
  pollfd p{};
  p.fd = s;
  p.events = POLLIN;
  return ::poll(&p, 1, ms) > 0 && (p.revents & POLLIN) != 0;
#endif
}

bool sendAll(int fd, const char* p, size_t n) {
  while (n > 0) {
    const int w = sockSend(fd, p, static_cast<int>(n));
    if (w <= 0) return false;
    p += w;
    n -= static_cast<size_t>(w);
  }
  return true;
}

// The local dotted-quad address whose interface routes toward `host`
// (the UDP-connect trick; a copy of the one in event_server.cpp — the
// cast socket layer stays self-contained per TU).
std::string localIpForPeer(const std::string& host) {
  sockaddr_in dst{};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(9);
  if (::inet_pton(AF_INET, host.c_str(), &dst.sin_addr) != 1) {
    if (host.empty()) return "";
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_NUMERICSERV;
    addrinfo* list = nullptr;
    if (getaddrinfo(host.c_str(), "9", &hints, &list) != 0 || list == nullptr)
      return "";
    std::memcpy(&dst, list->ai_addr, sizeof(dst));
    freeaddrinfo(list);
  }
  const int fd = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, 0));
  if (fd < 0) return "";
  std::string out;
  if (::connect(fd, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) == 0) {
    sockaddr_in local{};
    socklen_t len = sizeof(local);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
      char buf[INET_ADDRSTRLEN] = {0};
      if (::inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf)) != nullptr)
        out = buf;
    }
  }
  sockClose(fd);
  return out;
}

// Reads and discards the request head. Anything that manages a head is
// treated as a GET for the stream; the endpoint has exactly one resource.
bool readRequestHead(int fd) {
  std::string head;
  char buf[2048];
  while (head.find("\r\n\r\n") == std::string::npos && head.size() < 32 * 1024) {
    if (!waitReadable(fd, 5000)) return false;
    const int r = sockRecv(fd, buf, static_cast<int>(sizeof(buf)));
    if (r <= 0) return false;
    head.append(buf, static_cast<size_t>(r));
  }
  return true;
}

constexpr size_t kQueueBytes = 1 << 20;  // MPEG-TS bytes allowed in flight
constexpr int kStreamSendTimeoutSec = 10;

}  // namespace

TranscodeServer::~TranscodeServer() { stop(); }

const std::string& TranscodeServer::lastError() const {
  std::lock_guard<std::mutex> lock(error_mu_);
  return last_error_;
}

void TranscodeServer::recordError(const std::string& message) {
  std::lock_guard<std::mutex> lock(error_mu_);
  last_error_ = message;
}

bool TranscodeServer::pushBytes(const unsigned char* data, int size) {
  std::unique_lock<std::mutex> lock(bytes_mu_);
  // The queue cap is the flow control: a renderer reading slowly (or not
  // at all) backs up through the muxer into the transcode, then into the
  // bridge reads. stop() breaks the wait so the pump can always exit.
  bytes_cv_.wait(lock, [this] {
    return stopping_.load(std::memory_order_acquire) || queued_bytes_ < kQueueBytes;
  });
  if (stopping_.load(std::memory_order_acquire)) return false;
  queue_.emplace_back(reinterpret_cast<const char*>(data),
                      reinterpret_cast<const char*>(data) + size);
  queued_bytes_ += static_cast<size_t>(size);
  const bool first = !first_bytes_;
  first_bytes_ = true;
  lock.unlock();
  if (first) bytes_cv_.notify_all();  // waitReady's green light
  return true;
}

bool TranscodeServer::popBytes(std::vector<char>* out, size_t max, bool* eof) {
  std::unique_lock<std::mutex> lock(bytes_mu_);
  bytes_cv_.wait(lock, [this] {
    return stopping_.load(std::memory_order_acquire) || !queue_.empty() || eof_;
  });
  *eof = false;
  if (stopping_.load(std::memory_order_acquire)) return false;
  if (queue_.empty()) {  // eof_ after the drain
    *eof = true;
    return false;
  }
  while (!queue_.empty() && out->size() < max) {
    std::vector<char>& front = queue_.front();
    const size_t take = std::min(max - out->size(), front.size());
    out->insert(out->end(), front.begin(), front.begin() + static_cast<long>(take));
    front.erase(front.begin(), front.begin() + static_cast<long>(take));
    if (front.empty()) queue_.pop_front();
    queued_bytes_ -= take;
  }
  lock.unlock();
  bytes_cv_.notify_all();  // a blocked pump may have room again
  return true;
}

void TranscodeServer::acceptLoop(int listen_fd) {
  for (;;) {
    if (!running_.load(std::memory_order_acquire)) break;
    if (!waitReadable(listen_fd, 250)) continue;
    const int fd = static_cast<int>(::accept(listen_fd, nullptr, nullptr));
    if (fd < 0) {
      if (!running_.load(std::memory_order_acquire)) break;
      continue;
    }
    timeval tv{};
    tv.tv_sec = kStreamSendTimeoutSec;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv),
                 sizeof(tv));
    if (!readRequestHead(fd)) {
      sockClose(fd);
      continue;
    }
    bool expected = false;
    if (!stream_active_.compare_exchange_strong(expected, true)) {
      static const char kBusy[] =
          "HTTP/1.1 503 Service Unavailable\r\nCONNECTION: close\r\n"
          "CONTENT-LENGTH: 0\r\n\r\n";
      sendAll(fd, kBusy, sizeof(kBusy) - 1);
      sockClose(fd);
      continue;
    }
    {
      std::lock_guard<std::mutex> lock(stream_mu_);
      if (stream_thread_.joinable()) stream_thread_.join();
      stream_thread_ = std::thread([this, fd] { serveStream(fd); });
    }
  }
}

void TranscodeServer::serveStream(int fd) {
  static const char kHead[] =
      "HTTP/1.1 200 OK\r\nCONTENT-TYPE: video/mp2t\r\nCONNECTION: close\r\n\r\n";
  std::vector<char> buf;
  bool eof = false;
  if (sendAll(fd, kHead, sizeof(kHead) - 1)) {
    while (popBytes(&buf, 32 * 1024, &eof)) {
      if (!sendAll(fd, buf.data(), buf.size())) break;  // client went away
      buf.clear();
    }
  }
  sockClose(fd);
  stream_active_.store(false, std::memory_order_release);
}

#ifdef SOAR_WITH_FFMPEG

namespace {

// The muxer's AVIO write callback: the bytes go into the queue whose cap
// backpressures the whole pump. Only stop() makes pushBytes fail; the
// pump exits on its own then.
int queueWriteTrampoline(void* opaque, const unsigned char* buf, int buf_size) {
  auto* self = static_cast<TranscodeServer*>(opaque);
  return self->pushBytes(buf, buf_size) ? buf_size : -1;
}

// Copy decisions, read straight off the codec parameters: a stream the
// MPEG-TS pair already accepts travels untouched (no decoder is opened
// for it). Everything else is re-encoded to H.264 / AAC.
bool copyableVideo(const AVCodecParameters* par) {
  return par->codec_id == AV_CODEC_ID_H264 &&
         (par->format == AV_PIX_FMT_YUV420P || par->format == AV_PIX_FMT_YUVJ420P ||
          par->format == AV_PIX_FMT_NV12);
}
bool copyableAudio(const AVCodecParameters* par) {
  return par->codec_id == AV_CODEC_ID_AAC;
}

int evenSize(int v) { return v - (v % 2); }

struct PacketGuard {
  AVPacket* p = av_packet_alloc();
  ~PacketGuard() { av_packet_free(&p); }
};
struct FrameGuard {
  AVFrame* f = av_frame_alloc();
  ~FrameGuard() { av_frame_free(&f); }
};

// One pump pass owns every resource it opens (all released in teardown,
// whatever the outcome).
struct Pump {
  TranscodeServer* self = nullptr;
  std::string source;

  AVFormatContext* in = nullptr;
  AVCodecContext* dec_v = nullptr;  // opened only for re-encoded streams
  AVCodecContext* dec_a = nullptr;
  AVCodecContext* enc_v = nullptr;  // nullptr = the stream is copied
  AVCodecContext* enc_a = nullptr;
  AVFormatContext* out = nullptr;
  AVIOContext* pb = nullptr;
  SwsContext* sws = nullptr;
  SwrContext* swr = nullptr;
  int in_v = -1, in_a = -1, out_v = -1, out_a = -1;

  // Both output streams start at 0 (input stamps can be negative or
  // huge; the mpegts muxer rewrites from there).
  std::int64_t v_start = AV_NOPTS_VALUE;
  std::int64_t a_start = AV_NOPTS_VALUE;
  std::int64_t v_pts_counter = 0;  // NOPTS fallback for encoder input
  AVAudioFifo* fifo_a = nullptr;   // reframes decoded samples (any pacing)
                                   // into the encoder's fixed frame_size
  std::int64_t a_next_pts = 0;     // stamp of the fifo's read head
  bool a_pts_init = false;

  ~Pump() { teardown(); }

  void teardown() {
    avcodec_free_context(&dec_v);
    avcodec_free_context(&dec_a);
    avcodec_free_context(&enc_v);
    avcodec_free_context(&enc_a);
    av_audio_fifo_free(fifo_a);
    fifo_a = nullptr;
    sws_freeContext(sws);
    swr_free(&swr);
    if (out != nullptr) avformat_free_context(out);
    // The custom AVIO buffer and context outlive the muxer context (it
    // never owns them — CUSTOM_IO).
    if (pb != nullptr) {
      av_freep(&pb->buffer);
      avio_context_free(&pb);
    }
    if (in != nullptr) avformat_close_input(&in);
  }

  bool fail(const std::string& why) {
    self->recordError(why);
    return false;
  }

  bool openInput();
  bool openVideo();
  bool openAudio();
  bool openOutput();
  bool writeOut(AVPacket* pkt, int stream, AVRational src_tb);
  bool encodeVideo(AVFrame* frame);  // nullptr = flush
  bool encodeAudio(AVFrame* frame);  // nullptr = flush
  bool encodeOneAudioChunk(int samples);  // fifo → one encoder frame
  bool feedPacket(AVPacket* pkt);
};

bool Pump::openInput() {
  if (avformat_open_input(&in, source.c_str(), nullptr, nullptr) != 0)
    return fail("cast: transcode cannot open the source " + source);
  if (avformat_find_stream_info(in, nullptr) < 0)
    return fail("cast: transcode cannot read the source's stream layout");

  // The first real video and audio streams; attached cover art is not
  // the video. Subtitles and data streams do not travel.
  for (unsigned i = 0; i < in->nb_streams; ++i) {
    const AVStream* st = in->streams[i];
    if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && in_v < 0 &&
        (st->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0)
      in_v = static_cast<int>(i);
    if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && in_a < 0)
      in_a = static_cast<int>(i);
  }
  if (in_v < 0 && in_a < 0)
    return fail("cast: transcode found no video or audio stream in the source");

  if (in_v >= 0 && !copyableVideo(in->streams[in_v]->codecpar)) {
    const AVCodec* dec = avcodec_find_decoder(in->streams[in_v]->codecpar->codec_id);
    if (dec == nullptr)
      return fail("cast: transcode has no decoder for the video codec");
    dec_v = avcodec_alloc_context3(dec);
    if (dec_v == nullptr ||
        avcodec_parameters_to_context(dec_v, in->streams[in_v]->codecpar) < 0 ||
        avcodec_open2(dec_v, dec, nullptr) < 0)
      return fail("cast: transcode cannot open the video decoder");
  }
  if (in_a >= 0 && !copyableAudio(in->streams[in_a]->codecpar)) {
    const AVCodec* dec = avcodec_find_decoder(in->streams[in_a]->codecpar->codec_id);
    if (dec == nullptr)
      return fail("cast: transcode has no decoder for the audio codec");
    dec_a = avcodec_alloc_context3(dec);
    if (dec_a == nullptr ||
        avcodec_parameters_to_context(dec_a, in->streams[in_a]->codecpar) < 0 ||
        avcodec_open2(dec_a, dec, nullptr) < 0)
      return fail("cast: transcode cannot open the audio decoder");
  }
  return true;
}

bool Pump::openVideo() {
  if (in_v < 0 || dec_v == nullptr) return true;  // absent or copied
  AVStream* ist = in->streams[in_v];  // av_guess_frame_rate takes it mutable
  const AVCodec* enc = avcodec_find_encoder(AV_CODEC_ID_H264);
  if (enc == nullptr)
    return fail("cast: transcode has no H.264 encoder (a libx264-enabled FFmpeg is needed)");
  enc_v = avcodec_alloc_context3(enc);
  if (enc_v == nullptr) return fail("cast: transcode out of memory (video encoder)");
  AVRational fps = av_guess_frame_rate(in, ist, nullptr);
  if (fps.num <= 0 || fps.den <= 0) fps = AVRational{25, 1};
  enc_v->width = evenSize(dec_v->width);
  enc_v->height = evenSize(dec_v->height);
  enc_v->pix_fmt = AV_PIX_FMT_YUV420P;
  enc_v->time_base = AVRational{fps.den, fps.num};
  enc_v->framerate = fps;
  enc_v->gop_size = static_cast<int>(av_q2d(fps) * 2.0) + 1;  // ~2s keyframes
  enc_v->max_b_frames = 0;  // live TS: no reordering latency
  av_opt_set(enc_v->priv_data, "preset", "veryfast", 0);
  av_opt_set(enc_v->priv_data, "tune", "zerolatency", 0);
  av_opt_set(enc_v->priv_data, "crf", "23", 0);
  if (avcodec_open2(enc_v, enc, nullptr) < 0)
    return fail("cast: transcode cannot open the H.264 encoder");
  if (dec_v->pix_fmt != enc_v->pix_fmt || dec_v->width != enc_v->width ||
      dec_v->height != enc_v->height) {
    sws = sws_getContext(dec_v->width, dec_v->height, dec_v->pix_fmt, enc_v->width,
                         enc_v->height, enc_v->pix_fmt, SWS_BILINEAR, nullptr, nullptr,
                         nullptr);
    if (sws == nullptr) return fail("cast: transcode cannot scale the video");
  }
  return true;
}

bool Pump::openAudio() {
  if (in_a < 0 || dec_a == nullptr) return true;  // absent or copied
  const AVCodec* enc = avcodec_find_encoder(AV_CODEC_ID_AAC);
  if (enc == nullptr) return fail("cast: transcode has no AAC encoder");
  enc_a = avcodec_alloc_context3(enc);
  if (enc_a == nullptr) return fail("cast: transcode out of memory (audio encoder)");
  enc_a->sample_fmt = AV_SAMPLE_FMT_FLTP;
  enc_a->sample_rate = (dec_a->sample_rate >= 8000 && dec_a->sample_rate <= 96000)
                           ? dec_a->sample_rate
                           : 48000;
  if (dec_a->ch_layout.nb_channels > 0) {
    // Plain WAVE (and raw PCM) sources carry an unspecified layout; the
    // AAC encoder refuses those outright — apply the default order for the
    // channel count (stereo → FL+FR) before handing it over.
    if (dec_a->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
      av_channel_layout_default(&dec_a->ch_layout, dec_a->ch_layout.nb_channels);
    av_channel_layout_copy(&enc_a->ch_layout, &dec_a->ch_layout);
  } else {
    av_channel_layout_default(&enc_a->ch_layout, 2);
  }
  enc_a->time_base = AVRational{1, enc_a->sample_rate};
  if (avcodec_open2(enc_a, enc, nullptr) < 0)
    return fail("cast: transcode cannot open the AAC encoder");
  // Sized to one encoder frame; it grows as decoded samples arrive.
  // (The channel-layout overload of this probe exists only from FFmpeg
  // 7.1; the sample-format+channels form spans every supported version.)
  fifo_a = av_audio_fifo_alloc(enc_a->sample_fmt, enc_a->ch_layout.nb_channels,
                               enc_a->frame_size);
  if (fifo_a == nullptr)
    return fail("cast: transcode out of memory (audio queue)");
  const bool layouts_differ =
      av_channel_layout_compare(&dec_a->ch_layout, &enc_a->ch_layout) != 0;
  if (dec_a->sample_fmt != enc_a->sample_fmt ||
      dec_a->sample_rate != enc_a->sample_rate || layouts_differ) {
    SwrContext* raw = nullptr;
    if (swr_alloc_set_opts2(&raw, &enc_a->ch_layout, enc_a->sample_fmt,
                            enc_a->sample_rate, &dec_a->ch_layout, dec_a->sample_fmt,
                            dec_a->sample_rate, 0, nullptr) < 0 ||
        swr_init(raw) < 0) {
      swr_free(&raw);
      return fail("cast: transcode cannot resample the audio");
    }
    swr = raw;
  }
  return true;
}

bool Pump::openOutput() {
  if (avformat_alloc_output_context2(&out, nullptr, "mpegts", nullptr) < 0)
    return fail("cast: transcode cannot create the MPEG-TS muxer");
  // Output order: video first when present (renderers key off stream 0).
  if (in_v >= 0) {
    AVStream* ost = avformat_new_stream(out, nullptr);
    if (ost == nullptr) return fail("cast: transcode out of memory (video stream)");
    if (enc_v != nullptr) {
      ost->time_base = enc_v->time_base;
      if (avcodec_parameters_from_context(ost->codecpar, enc_v) < 0)
        return fail("cast: transcode cannot set the video stream format");
    } else if (avcodec_parameters_copy(ost->codecpar, in->streams[in_v]->codecpar) < 0) {
      return fail("cast: transcode cannot set the video stream format");
    } else {
      ost->codecpar->codec_tag = 0;
    }
    out_v = static_cast<int>(ost->index);
  }
  if (in_a >= 0) {
    AVStream* ost = avformat_new_stream(out, nullptr);
    if (ost == nullptr) return fail("cast: transcode out of memory (audio stream)");
    if (enc_a != nullptr) {
      ost->time_base = enc_a->time_base;
      if (avcodec_parameters_from_context(ost->codecpar, enc_a) < 0)
        return fail("cast: transcode cannot set the audio stream format");
    } else if (avcodec_parameters_copy(ost->codecpar, in->streams[in_a]->codecpar) < 0) {
      return fail("cast: transcode cannot set the audio stream format");
    } else {
      ost->codecpar->codec_tag = 0;
    }
    out_a = static_cast<int>(ost->index);
  }

  unsigned char* io_buf = static_cast<unsigned char*>(av_malloc(32 * 1024));
  if (io_buf == nullptr) return fail("cast: transcode out of memory (stream buffer)");
  pb = avio_alloc_context(io_buf, 32 * 1024, 1, self, nullptr,
                          &queueWriteTrampoline, nullptr);
  if (pb == nullptr) {
    av_free(io_buf);
    return fail("cast: transcode out of memory (stream io)");
  }
  out->pb = pb;
  out->flags |= AVFMT_FLAG_CUSTOM_IO;
  if (avformat_write_header(out, nullptr) < 0)
    return fail("cast: transcode cannot write the MPEG-TS header");
  return true;
}

bool Pump::writeOut(AVPacket* pkt, int stream, AVRational src_tb) {
  av_packet_rescale_ts(pkt, src_tb, out->streams[stream]->time_base);
  pkt->stream_index = stream;
  if (av_interleaved_write_frame(out, pkt) >= 0) return true;
  fail("cast: transcode muxing failed");
  return false;
}

bool Pump::encodeVideo(AVFrame* frame) {
  if (avcodec_send_frame(enc_v, frame) < 0) {
    fail("cast: transcode video encode failed");
    return false;
  }
  PacketGuard pkt;
  for (;;) {
    const int rc = avcodec_receive_packet(enc_v, pkt.p);
    if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
    if (rc < 0) {
      fail("cast: transcode video encode failed");
      return false;
    }
    if (!writeOut(pkt.p, out_v, enc_v->time_base)) return false;
  }
}

bool Pump::encodeAudio(AVFrame* frame) {
  if (avcodec_send_frame(enc_a, frame) < 0) {
    fail("cast: transcode audio encode failed");
    return false;
  }
  PacketGuard pkt;
  for (;;) {
    const int rc = avcodec_receive_packet(enc_a, pkt.p);
    if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
    if (rc < 0) {
      fail("cast: transcode audio encode failed");
      return false;
    }
    if (!writeOut(pkt.p, out_a, enc_a->time_base)) return false;
  }
}

bool Pump::encodeOneAudioChunk(int samples) {
  FrameGuard out;
  out.f->format = enc_a->sample_fmt;
  out.f->sample_rate = enc_a->sample_rate;
  av_channel_layout_copy(&out.f->ch_layout, &enc_a->ch_layout);
  out.f->nb_samples = samples;
  if (av_frame_get_buffer(out.f, 0) < 0 ||
      av_audio_fifo_read(fifo_a, (void**)out.f->data, samples) < samples) {
    fail("cast: transcode out of memory (audio frame)");
    return false;
  }
  out.f->pts = a_next_pts;
  a_next_pts += samples;
  return encodeAudio(out.f);
}

bool Pump::feedPacket(AVPacket* pkt) {
  const int idx = pkt->stream_index;
  if (idx == in_v) {
    if (v_start == AV_NOPTS_VALUE && pkt->pts != AV_NOPTS_VALUE) v_start = pkt->pts;
    if (enc_v == nullptr) {  // copied through untouched
      if (v_start != AV_NOPTS_VALUE) {
        if (pkt->pts != AV_NOPTS_VALUE) pkt->pts -= v_start;
        if (pkt->dts != AV_NOPTS_VALUE) pkt->dts -= v_start;
      }
      return writeOut(pkt, out_v, in->streams[in_v]->time_base);
    }
    FrameGuard frame;
    if (avcodec_send_packet(dec_v, pkt) < 0) {
      fail("cast: transcode video decode failed");
      return false;
    }
    for (;;) {
      const int rc = avcodec_receive_frame(dec_v, frame.f);
      if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
      if (rc < 0) {
        fail("cast: transcode video decode failed");
        return false;
      }
      AVFrame* dest = frame.f;
      FrameGuard scaled;
      if (sws != nullptr) {
        scaled.f->format = enc_v->pix_fmt;
        scaled.f->width = enc_v->width;
        scaled.f->height = enc_v->height;
        if (av_frame_get_buffer(scaled.f, 0) < 0) {
          fail("cast: transcode out of memory (video frame)");
          return false;
        }
        sws_scale(sws, frame.f->data, frame.f->linesize, 0, dec_v->height,
                  scaled.f->data, scaled.f->linesize);
        scaled.f->pts = frame.f->pts;
        dest = scaled.f;
      }
      if (dest->pts != AV_NOPTS_VALUE && v_start != AV_NOPTS_VALUE)
        dest->pts = av_rescale_q(dest->pts - v_start, in->streams[in_v]->time_base,
                                 enc_v->time_base);
      else if (dest->pts == AV_NOPTS_VALUE)
        dest->pts = v_pts_counter;
      ++v_pts_counter;
      if (!encodeVideo(dest)) return false;
    }
  }
  if (idx == in_a) {
    if (a_start == AV_NOPTS_VALUE && pkt->pts != AV_NOPTS_VALUE) a_start = pkt->pts;
    if (enc_a == nullptr) {
      if (a_start != AV_NOPTS_VALUE) {
        if (pkt->pts != AV_NOPTS_VALUE) pkt->pts -= a_start;
        if (pkt->dts != AV_NOPTS_VALUE) pkt->dts -= a_start;
      }
      return writeOut(pkt, out_a, in->streams[in_a]->time_base);
    }
    FrameGuard frame;
    if (avcodec_send_packet(dec_a, pkt) < 0) {
      fail("cast: transcode audio decode failed");
      return false;
    }
    for (;;) {
      const int rc = avcodec_receive_frame(dec_a, frame.f);
      if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
      if (rc < 0) {
        fail("cast: transcode audio decode failed");
        return false;
      }
      AVFrame* dest = frame.f;
      FrameGuard converted;
      if (swr != nullptr) {
        converted.f->format = enc_a->sample_fmt;
        converted.f->sample_rate = enc_a->sample_rate;
        av_channel_layout_copy(&converted.f->ch_layout, &enc_a->ch_layout);
        // The output buffer is sized for the converter's upper bound —
        // av_frame_get_buffer allocates audio by nb_samples, and a zero
        // would silently cap swr_convert at nothing.
        converted.f->nb_samples = swr_get_out_samples(swr, frame.f->nb_samples);
        if (converted.f->nb_samples <= 0 ||
            av_frame_get_buffer(converted.f, 0) < 0) {
          fail("cast: transcode out of memory (audio frame)");
          return false;
        }
        const int n = swr_convert(swr, converted.f->data, converted.f->nb_samples,
                                  const_cast<const uint8_t**>(frame.f->data),
                                  frame.f->nb_samples);
        if (n < 0) {
          fail("cast: transcode audio resample failed");
          return false;
        }
        if (n == 0) continue;  // swr buffered the input; nothing to encode yet
        converted.f->nb_samples = n;
        dest = converted.f;
      }
      if (dest->pts != AV_NOPTS_VALUE && a_start != AV_NOPTS_VALUE)
        dest->pts = av_rescale_q(dest->pts - a_start,
                                 in->streams[in_a]->time_base, enc_a->time_base);
      if (!a_pts_init) {
        // The fifo decides frame boundaries, so the stamp travels with
        // the first sample that enters it (samples == ticks at 1/rate).
        a_next_pts = dest->pts != AV_NOPTS_VALUE ? dest->pts : 0;
        a_pts_init = true;
      }
      if (av_audio_fifo_write(fifo_a, (void**)dest->data, dest->nb_samples) <
          dest->nb_samples) {
        fail("cast: transcode out of memory (audio queue)");
        return false;
      }
      // The encoder consumes fixed frame_size chunks — decoded frames
      // arrive in the source's own sizes (mp3's 1152 ≠ aac's 1024), so
      // the fifo is what keeps send_frame legal.
      while (av_audio_fifo_size(fifo_a) >= enc_a->frame_size)
        if (!encodeOneAudioChunk(enc_a->frame_size)) return false;
    }
  }
  return true;  // subtitles / data streams do not travel
}

}  // namespace

void TranscodeServer::pumpLoop() {
  // The demuxer may follow http:// sources (the bridge URL); the player
  // backend initializes libavformat's network layer at startup, but a
  // cast test binary never instantiates it — init here is idempotent.
  static const bool net = [] {
    avformat_network_init();
    return true;
  }();
  (void)net;

  Pump pump;
  pump.self = this;
  pump.source = source_url_;

  const bool ok = [&] {
    if (!pump.openInput() || !pump.openVideo() || !pump.openAudio() ||
        !pump.openOutput())
      return false;
    PacketGuard pkt;
    while (!stopping_.load(std::memory_order_acquire)) {
      const int rc = av_read_frame(pump.in, pkt.p);
      if (rc == AVERROR_EOF) break;
      if (rc < 0) {
        if (stopping_.load(std::memory_order_acquire)) return false;
        pump.fail("cast: transcode cannot read the source stream");
        return false;
      }
      const bool fed = pump.feedPacket(pkt.p);
      av_packet_unref(pkt.p);
      if (!fed) return false;
    }
    if (stopping_.load(std::memory_order_acquire)) return false;
    // EOF: pull swr's internal residue into the fifo, drain every whole
    // chunk, and send the sub-chunk tail as the small last frame (the
    // AAC encoder takes one; encoders that do not lose at most
    // frame_size-1 samples ≈ 23ms of stream end).
    if (pump.fifo_a != nullptr) {
      if (pump.swr != nullptr) {
        for (;;) {
          FrameGuard tail;
          tail.f->format = pump.enc_a->sample_fmt;
          tail.f->sample_rate = pump.enc_a->sample_rate;
          av_channel_layout_copy(&tail.f->ch_layout, &pump.enc_a->ch_layout);
          tail.f->nb_samples = 4096;  // one generous pull
          if (av_frame_get_buffer(tail.f, 0) < 0) {
            pump.fail("cast: transcode out of memory (audio frame)");
            return false;
          }
          const int n = swr_convert(pump.swr, tail.f->data, tail.f->nb_samples,
                                    nullptr, 0);
          if (n < 0) {
            pump.fail("cast: transcode audio resample failed");
            return false;
          }
          if (n == 0) break;
          if (av_audio_fifo_write(pump.fifo_a, (void**)tail.f->data, n) < n) {
            pump.fail("cast: transcode out of memory (audio queue)");
            return false;
          }
        }
      }
      while (av_audio_fifo_size(pump.fifo_a) > 0) {
        const int chunk =
            std::min(av_audio_fifo_size(pump.fifo_a), pump.enc_a->frame_size);
        if (chunk < pump.enc_a->frame_size &&
            (pump.enc_a->codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME) == 0)
          break;  // the encoder refuses a short tail; it is negligible
        if (!pump.encodeOneAudioChunk(chunk)) return false;
      }
    }
    // Drain both encoders, flush the interleave queue and the io buffer —
    // a clean end a renderer sees as a normal stream close.
    if (pump.enc_v != nullptr && !pump.encodeVideo(nullptr)) return false;
    if (pump.enc_a != nullptr && !pump.encodeAudio(nullptr)) return false;
    if (av_interleaved_write_frame(pump.out, nullptr) < 0 ||
        av_write_trailer(pump.out) < 0) {
      pump.fail("cast: transcode muxing failed at the end");
      return false;
    }
    avio_flush(pump.pb);
    return true;
  }();

  {
    std::lock_guard<std::mutex> lock(error_mu_);
    if (!ok && !stopping_.load(std::memory_order_acquire) && last_error_.empty())
      last_error_ = "cast: transcode failed";
  }
  {
    std::lock_guard<std::mutex> lock(bytes_mu_);
    eof_ = true;
  }
  bytes_cv_.notify_all();  // waitReady and a connected serve thread wake
}

#else  // no FFmpeg backend: the inert stub

void TranscodeServer::pumpLoop() {}  // never started

#endif  // SOAR_WITH_FFMPEG

bool TranscodeServer::start(const std::string& source_url, std::string* err) {
  if (running_.load(std::memory_order_acquire)) return true;
#ifdef SOAR_WITH_FFMPEG
  sockInit();
  const int fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
  if (fd < 0) {
    *err = "cast: transcode server cannot create socket (errno " +
           std::to_string(lastNetError()) + ")";
    return false;
  }
  const int reuse = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
               sizeof(reuse));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = 0;  // ephemeral: urlFor() is built after bind
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
      ::listen(fd, 2) != 0) {
    *err = "cast: transcode server cannot bind (errno " +
           std::to_string(lastNetError()) + ")";
    sockClose(fd);
    return false;
  }
  sockaddr_in bound{};
  socklen_t len = sizeof(bound);
  ::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len);
  listen_fd_ = fd;
  port_ = ntohs(bound.sin_port);
  source_url_ = source_url;
  running_.store(true, std::memory_order_release);
  accept_thread_ = std::thread([this, fd = listen_fd_] { acceptLoop(fd); });
  pump_thread_ = std::thread([this] { pumpLoop(); });
  return true;
#else
  (void)source_url;
  *err = "cast: sender-side transcoding needs a build with the FFmpeg backend "
         "(this one was built without it)";
  return false;
#endif
}

bool TranscodeServer::waitReady(int timeout_ms) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  std::unique_lock<std::mutex> lock(bytes_mu_);
  while (!first_bytes_ && !eof_ && !stopping_.load(std::memory_order_acquire)) {
    if (bytes_cv_.wait_until(lock, deadline) == std::cv_status::timeout) break;
  }
  // Readiness is "the muxer header went out", full stop. A short source
  // can have the whole transcode in the queue and eof_ set before the
  // caller gets here — that is a ready stream that merely ends, not a
  // failure; only "no bytes ever" (broken source, stopped, timeout) is.
  return first_bytes_;
}

std::string TranscodeServer::urlFor(const std::string& peer_host) const {
  if (port_ == 0) return "";
  const std::string local = localIpForPeer(peer_host);
  if (local.empty()) return "";
  return "http://" + local + ":" + std::to_string(port_) + "/";
}

void TranscodeServer::stop() {
  if (!running_.exchange(false, std::memory_order_acq_rel)) return;
  stopping_.store(true, std::memory_order_release);
  if (listen_fd_ >= 0) {
#ifdef _WIN32
    ::shutdown(listen_fd_, SD_BOTH);
#else
    ::shutdown(listen_fd_, SHUT_RDWR);
#endif
    // The close itself is the accept wake on Darwin (shutdown on a
    // listener is a no-op there).
    sockClose(listen_fd_);
    listen_fd_ = -1;
  }
  bytes_cv_.notify_all();  // unblock the pump's push and the serve's pop
  if (pump_thread_.joinable()) pump_thread_.join();
  if (accept_thread_.joinable()) accept_thread_.join();
  // The accept loop is gone, so no respawn can race this final join.
  {
    std::lock_guard<std::mutex> lock(stream_mu_);
    if (stream_thread_.joinable()) stream_thread_.join();
  }
  port_ = 0;
  source_url_.clear();
  {
    std::lock_guard<std::mutex> lock(bytes_mu_);
    queue_.clear();
    queued_bytes_ = 0;
    first_bytes_ = false;
    eof_ = false;
  }
}

}  // namespace soar::cast
