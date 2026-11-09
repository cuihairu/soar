// ffmpeg_backend_internal.h: the FFmpegBackend implementation's
// shared internals - the libav/SDL include set, the file-scope
// helpers and the nested-state struct definitions. Internal to the
// backend translation units; not installed. Split out of
// ffmpeg_backend.cpp so the per-concern files stay readable.

#ifndef SOAR_CORE_FFMPEG_BACKEND_INTERNAL_H_
#define SOAR_CORE_FFMPEG_BACKEND_INTERNAL_H_

#include "soar/core/ffmpeg_backend.h"

#include <fmt/format.h>

#include "soar/core/ass_dialogue.h"
#include "soar/core/http_cache.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/time.h>
#include <libavutil/timestamp.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string_view>

#ifdef SOAR_WITH_SDL2
#  include <SDL.h>
#endif


namespace soar {

namespace {

constexpr std::size_t kMaxVideoQueueFrames = 6;
constexpr std::size_t kMaxAudioQueueFrames = 32;
constexpr std::size_t kMaxSubtitleQueueFrames = 4;
constexpr auto kPositionEmitGranularity = std::chrono::milliseconds(200);
// Network stall watchdog: a read that stays quiet longer than
// kNetworkStallReportMs reports BufferingStarted, and only a stall past
// kNetworkStallLimitMs aborts the read into Error. The abort deliberately
// waits out the tolerance window because killing a read kills the
// connection; a merely slow source recovers on the same socket.
constexpr auto kNetworkStallReportMs = std::chrono::milliseconds(10'000);
constexpr auto kNetworkStallLimitMs = std::chrono::milliseconds(60'000);

// A stop() must not stall out on a source that never answers: the rewind
// seek at the end of stop() is the only avio outside a watched read, so it
// gets this budget instead of the decode read's stall tolerance.
constexpr std::chrono::milliseconds kStopRewindLimit = std::chrono::milliseconds(2000);

// The stream-less verdict shared by the open and scan report sites: a
// container with nothing playable is judged by its content, not by which
// build stage first refuses it (the transcode pump folds the same EEXIST
// case into its kNoStreamsVerdict).
constexpr const char* kNoStreamsVerdict = "no video or audio stream found";

// HDR transfer characteristics: SMPTE2084 (HDR10) and ARIB-STD-B67 (HLG)
// both encode luma above the SDR reference, so such frames need the
// tone-map graph before the render path can show them at all (docs/mvp.md
// §3). Primaries and matrix are not part of the test: the graph converts
// both, and the transfer alone says "HDR source".
inline bool isHdrTransfer(AVColorTransferCharacteristic trc) {
  return trc == AVCOL_TRC_SMPTE2084 || trc == AVCOL_TRC_ARIB_STD_B67;
}

// Steady-clock ms since an unspecified epoch: the shared clock for the
// network stall watchdog and the bounded-io deadline above.
static std::chrono::milliseconds::rep steadyNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Hardware decode (docs/mvp.md §3). The get_format callback picks the
// hwaccel pixel format the backend negotiated from the decoder's offered
// list; which one is wanted is carried per-instance through
// AVCodecContext::opaque (libavcodec never touches that field), so
// concurrently opening backend instances cannot cross-talk.

// The get_format callback installed on the video decoder: picks the
// hwaccel pixel format the backend negotiated out of the decoder's offered
// list. The wanted format rides in AVCodecContext::opaque as a pointer to
// the instance's hw_pix_fmt_ (libavcodec never reads or writes that
// field), so concurrently opening backend instances cannot cross-talk.
enum AVPixelFormat hwGetFormat(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts) {
  const int* target = ctx ? static_cast<const int*>(ctx->opaque) : nullptr;
  if (!target || *target < 0) {
    return AV_PIX_FMT_NONE;
  }
  for (const enum AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
    if (static_cast<int>(*p) == *target) {
      return *p;
    }
  }
  return AV_PIX_FMT_NONE;
}

// Map a --hwdec= / MediaSource::hwdec value to the FFmpeg hwaccel device
// types to try, in order. "auto" resolves to the platform's usual set; a
// specific name maps to one device. Unknown names yield an empty list,
// which the caller reads as "no hwaccel" (software only).
std::vector<AVHWDeviceType> hwdecDevices(const std::string& hwdec) {
  std::string v = hwdec;
  for (char& c : v) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  if (v == "auto") {
#if defined(__linux__)
    return {AV_HWDEVICE_TYPE_VAAPI, AV_HWDEVICE_TYPE_VDPAU};
#elif defined(_WIN32)
    return {AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_DXVA2};
#elif defined(__APPLE__)
    return {AV_HWDEVICE_TYPE_VIDEOTOOLBOX};
#else
    return {};
#endif
  }
  if (v == "vaapi") return {AV_HWDEVICE_TYPE_VAAPI};
  if (v == "vdpau") return {AV_HWDEVICE_TYPE_VDPAU};
  if (v == "d3d11" || v == "d3d11va") return {AV_HWDEVICE_TYPE_D3D11VA};
  if (v == "dxva2") return {AV_HWDEVICE_TYPE_DXVA2};
  if (v == "videotoolbox") return {AV_HWDEVICE_TYPE_VIDEOTOOLBOX};
  if (v == "cuda" || v == "nvdec") return {AV_HWDEVICE_TYPE_CUDA};
  return {};
}

std::string dictValue(AVDictionary* dict, const char* key) {
  if (!dict || !key) {
    return {};
  }
  const AVDictionaryEntry* entry = av_dict_get(dict, key, nullptr, 0);
  if (!entry || !entry->value) {
    return {};
  }
  return std::string(entry->value);
}

std::string codecNameFromCodecId(AVCodecID codec_id) {
  const AVCodec* codec = avcodec_find_decoder(codec_id);
  if (codec && codec->name) {
    return std::string(codec->name);
  }
  return fmt::format("codec({})", static_cast<int>(codec_id));
}

// ASS event text extraction. An ASS rect payload is a script section whose
// event lines read "Dialogue: layer,start,end,style,name,mL,mR,mV,effect,
// text" — the human payload starts after the ninth comma. Override blocks
// {...} are dropped and hard breaks (\N, \n) become spaces, so an SRT line
// like "Hello" surfaces as plain "Hello" (the SRT/WebVTT decoders emit
// ASS-format rects; see processSubtitleFrame).
// The Dialogue-line reassembly (assTimestamp/assDialogueLine) lives in
// ass_dialogue.{h,cpp} as pure functions so the tolerant-parsing arms are
// unit-testable headlessly (docs/mvp.md §6).

// The ff_ass_get_dialog decoder wraps synthesized events in brackets:
// "[readorder,layer,style,speaker,mL,mR,mV,effect,text]" — 8 commas, 9 fields.
std::string assDialogueText(const char* ass) {
  if (ass == nullptr) {
    return {};
  }
  std::string_view s(ass);
  // Strip surrounding brackets if present (ff_ass_get_dialog format).
  // ff_ass_get_dialog emits 8‑comma text like [readorder,layer,style,speaker,
  // 0,0,0,,text] — the surrounding [ ] are stripped so the payload after the
  // eighth comma is the plain text.  The ASS decoder passes full
  // "Dialogue:..." lines through without surrounding brackets, and SRT/WebVTT
  // synthesizers produce 8‑comma text without brackets; this code is a defensive
  // no‑op for those paths but handles the occasional bracketed wrapper.
  if (!s.empty()) {
    if (s.front() == '[') {
      s.remove_prefix(1);
    }
    if (!s.empty() && s.back() == ']') {
      s.remove_suffix(1);
    }
  }
  std::string out;
  std::size_t pos = 0;
  while (pos < s.size()) {
    const std::size_t eol = s.find('\n', pos);
    const std::size_t line_end = eol == std::string_view::npos ? s.size() : eol;
    std::string_view line = s.substr(pos, line_end - pos);
    pos = eol == std::string_view::npos ? s.size() : eol + 1;
    const bool full_event = line.rfind("Dialogue:", 0) == 0;
    const int commas_needed = full_event ? 9 : 8;
    int commas = 0;
    std::size_t i = 0;
    while (i < line.size() && commas < commas_needed) {
      if (line[i] == ',') {
        ++commas;
      }
      ++i;
    }
    if (commas < commas_needed) {
      continue;
    }
    bool in_braces = false;
    for (; i < line.size(); ++i) {
      const char c = line[i];
      if (in_braces) {
        if (c == '}') {
          in_braces = false;
        }
        continue;
      }
      if (c == '{') {
        in_braces = true;
        continue;
      }
      if (c == '\\' && i + 1 < line.size() && (line[i + 1] == 'N' || line[i + 1] == 'n')) {
        out.push_back(' ');
        ++i;
        continue;
      }
      // \N at end of line: also convert to space for robust handling
      if (c == '\\' && i + 1 >= line.size()) {
        out.push_back(' ');
        continue;
      }
      out.push_back(c);
    }
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '\r' || out.back() == ']')) {
    out.pop_back();
  }
  return out;
}

int pickBestStreamIndex(AVFormatContext* ctx, AVMediaType type) {
  if (!ctx) {
    return -1;
  }
  int first = -1;
  int best = -1;
  for (unsigned int i = 0; i < ctx->nb_streams; ++i) {
    AVStream* stream = ctx->streams[i];
    if (!stream || !stream->codecpar) {
      continue;
    }
    if (stream->codecpar->codec_type != type) {
      continue;
    }
    if (first < 0) {
      first = static_cast<int>(i);
    }
    if ((stream->disposition & AV_DISPOSITION_DEFAULT) != 0) {
      best = static_cast<int>(i);
      break;
    }
  }
  return best >= 0 ? best : first;
}

void freeFrame(AVFrame*& frame) {
  if (!frame) {
    return;
  }
  av_frame_free(&frame);
  frame = nullptr;
}

template <typename Queue>
void clearDecodedQueue(Queue& q) {
  while (!q.empty()) {
    auto item = q.front();
    q.pop();
    freeFrame(item.frame);
  }
}

} // namespace


struct FFmpegBackend::VideoConvertState {
  int width{0};
  int height{0};
  AVPixelFormat src_fmt{AV_PIX_FMT_NONE};
  AVFrame* dst{nullptr};
  std::vector<std::uint8_t> buffer;

  ~VideoConvertState() {
    if (dst) {
      av_frame_free(&dst);
      dst = nullptr;
    }
  }
};


// HDR → SDR tone map (docs/mvp.md §3): a filter graph built once on the
// first HDR frame with that frame's geometry, used for every frame after.
// The destructor releases the graph, which owns both end contexts, so that
// alone is the whole teardown. Decode-thread only, like video_convert_;
// released in cleanupDecoders next to it.
struct FFmpegBackend::HdrToneMap {
  AVFilterGraph* graph{nullptr};
  AVFilterContext* src{nullptr};
  AVFilterContext* sink{nullptr};

  ~HdrToneMap() {
    avfilter_graph_free(&graph);
  }
};


namespace {

// The tone-map recipe, a fixed string so tests can reason about its output
// (the fixture's measured luma window is in the test file). It is zimg's
// zscale pipeline: linearize at 100 nits (the SDR reference), convert to
// BT.709 primaries, hable tone map, then re-encode to BT.709 gamma. The
// alternatives were rejected on measurement: the stock tonemap filter
// needs linear-light input, and a graph without zscale has no EOTF step,
// so it warns "Tonemapping works on linear light only" and tone maps the
// raw HDR code values — a wrong picture. A build without libzimg therefore
// passes HDR frames through untouched (HdrToneMapState::PassThrough)
// rather than shipping that.

constexpr const char* kHdrToneMapZscale =
    "zscale=t=linear:npl=100,format=gbrpf32le,zscale=p=bt709,"
    "tonemap=tonemap=hable:desat=0,zscale=t=bt709:m=bt709:r=tv,format=yuv420p";

} // namespace


#ifdef SOAR_WITH_SDL2
struct FFmpegBackend::SDLAudio {
  SDL_AudioDeviceID dev{0};
  SDL_AudioSpec obtained{};
  int sample_rate{0};
  int channels{0};
  // Endpoint currently open (or last opened): "" = system default. Part
  // of the reuse check — a device switch must re-open even when the
  // stream parameters did not change.
  std::string device;

  // Everything here is reachable from the decode thread (open, queue,
  // backpressure probe) and from whoever calls stop()/close() — the UI
  // thread — so the struct guards itself. The lock is not contended in
  // practice: a device switch does not close anything, it only changes the
  // name the next ensureOpen() compares against. Mutable: the queue-depth
  // probe is a const read that still has to serialize.
  mutable std::mutex mtx;

  bool ensureOpen(int target_rate, int target_channels, const std::string& device_name, std::string& err) {
    std::lock_guard<std::mutex> lock(mtx);
    if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
      if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        err = SDL_GetError();
        return false;
      }
    }

    if (dev != 0 && sample_rate == target_rate && channels == target_channels && device == device_name) {
      return true;
    }

    closeLocked();

    SDL_AudioSpec wanted{};
    wanted.freq = target_rate;
    wanted.format = AUDIO_F32SYS;
    wanted.channels = static_cast<Uint8>(std::clamp(target_channels, 1, 8));
    wanted.samples = 2048;
    wanted.callback = nullptr; // queued audio

    dev = SDL_OpenAudioDevice(device_name.empty() ? nullptr : device_name.c_str(), 0, &wanted,
                              &obtained, SDL_AUDIO_ALLOW_FORMAT_CHANGE);
    if (dev == 0) {
      err = SDL_GetError();
      return false;
    }

    sample_rate = obtained.freq;
    channels = obtained.channels;
    device = device_name;
    SDL_PauseAudioDevice(dev, 0);
    return true;
  }

  // There is deliberately no "reopen for another device" entry point: the
  // switch is a name change plus the next ensureOpen() from the decode
  // thread, which re-opens under this same lock. Doing it in the UI
  // thread would mean closing an endpoint while a queue is draining.

  void clear() {
    std::lock_guard<std::mutex> lock(mtx);
    if (dev != 0) {
      SDL_ClearQueuedAudio(dev);
    }
  }

  void close() {
    std::lock_guard<std::mutex> lock(mtx);
    closeLocked();
  }

  void closeLocked() {
    if (dev != 0) {
      SDL_CloseAudioDevice(dev);
      dev = 0;
    }
    sample_rate = 0;
    channels = 0;
    obtained = SDL_AudioSpec{};
    device.clear();
  }

  std::size_t queuedBytes() const {
    std::lock_guard<std::mutex> lock(mtx);
    if (dev == 0) {
      return 0;
    }
    return SDL_GetQueuedAudioSize(dev);
  }

  bool queue(const void* data, std::size_t bytes, std::string& err) {
    std::lock_guard<std::mutex> lock(mtx);
    if (dev == 0) {
      err = "audio device not open";
      return false;
    }
    if (SDL_QueueAudio(dev, data, static_cast<Uint32>(bytes)) != 0) {
      err = SDL_GetError();
      return false;
    }
    return true;
  }
};

#else
struct FFmpegBackend::SDLAudio {
  bool ensureOpen(int, int, const std::string&, std::string&) { return false; }
  void clear() {}
  void close() {}
  std::size_t queuedBytes() const { return 0; }
  bool queue(const void*, std::size_t, std::string&) { return false; }
};
#endif


//=============================================================================
// FFmpeg Context Management
//=============================================================================

// Defined here (not in the header) so HttpCache and the FFmpeg buffer
// pointers stay implementation details. `pos` is the logical read offset
// the sequential read callback serves from; avio keeps it in sync via the
// seek callback.
struct FFmpegBackend::AvioCacheContext {
  std::unique_ptr<HttpCache> cache;
  AVIOContext* pb = nullptr;
  uint8_t* buffer = nullptr;  // owned by pb after avio_alloc_context
  uint64_t pos = 0;
  // The static avio callbacks need the backend for the progress emitter's
  // decode-loop gate and emit(); set at the one construction site, so it is
  // never null.
  FFmpegBackend* owner = nullptr;
  // Download-progress throttle state (P3c). Confined to whichever thread
  // currently owns cache reads — the decode loop while playing, the seek
  // caller otherwise — mirroring HttpCache's own one-reader-at-a-time
  // contract, so a plain field is enough.
  bool progress_calibrated = false;
  uint64_t last_progress_step = 0;
};

} // namespace soar

#endif  // SOAR_CORE_FFMPEG_BACKEND_INTERNAL_H_
