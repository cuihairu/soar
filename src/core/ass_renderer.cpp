#include "soar/core/ass_renderer.h"

#ifdef SOAR_WITH_LIBASS
#  include <ass/ass.h>
#  include <mutex>
#endif

#include <algorithm>
#include <cstring>

namespace soar {
namespace {

#ifdef SOAR_WITH_LIBASS
// Straight-alpha source-over of one glyph bitmap onto the RGBA canvas.
// ASS_Image color packs 0xRRGGBBAA where the low byte is alpha in the
// ASS convention: 0 = opaque, 255 = transparent — the inverse of the
// straight alpha this canvas speaks (mpv reads it the same way). The
// bitmap bytes are per-pixel coverage; the two combine multiplicatively,
// then the usual `out = src + dst * (1 - src_a)` compositing applies.
// The canvas starts fully transparent each render, so first-pixel is the
// simple case and only out_alpha needs the general form.
void blendGlyph(std::vector<std::uint8_t>& canvas, int cw, int ch,
                const ASS_Image* img) {
  if (img->w <= 0 || img->h <= 0 || img->bitmap == nullptr) return;
  const int x0 = img->dst_x;
  const int y0 = img->dst_y;
  // libass clips to the frame size, but stay defensive: a glyph that
  // starts off-canvas must not index out of bounds.
  if (x0 >= cw || y0 >= ch) return;

  const std::uint32_t color = img->color;
  const int rgb[3] = {static_cast<int>((color >> 24) & 0xFF),
                      static_cast<int>((color >> 16) & 0xFF),
                      static_cast<int>((color >> 8) & 0xFF)};
  const int ga = 255 - static_cast<int>(color & 0xFF);  // 0=opaque → invert
  if (ga == 0) return;  // fully transparent glyph

  const int x_end = std::min(x0 + img->w, cw);
  const int y_end = std::min(y0 + img->h, ch);
  // The last bitmap row may be unpadded; the guaranteed allocation is
  // `(stride * (h - 1)) + w` bytes, so clamp each row's read span to what
  // is left of that budget after its base offset.
  const std::size_t alloc =
      static_cast<std::size_t>(img->stride) * (img->h - 1) + img->w;
  for (int y = y0; y < y_end; ++y) {
    const int row = y - y0;
    const std::size_t base = static_cast<std::size_t>(row) * img->stride;
    const int readable = static_cast<int>(
        std::min<std::size_t>(x_end - x0, alloc - base));
    auto* dst = canvas.data() + (static_cast<std::size_t>(y) * cw + x0) * 4;
    const unsigned char* src = img->bitmap + base;
    for (int i = 0; i < readable; ++i) {
      const int sa = src[i] * ga / 255;
      if (sa == 0) continue;
      const int da = dst[i * 4 + 3];
      // sa >= 1 here, so omega = sa + da*(255-sa)/255 can never be zero.
      const int oa = sa + da * (255 - sa) / 255;
      for (int c = 0; c < 3; ++c) {
        const int dc = dst[i * 4 + c];
        // Rounding can push the exact result to 256 (e.g. a low-coverage
        // glyph over a nearly identical one); wrap-around here would paint
        // black — clamp instead.
        dst[i * 4 + c] = static_cast<std::uint8_t>(std::min(
            255, (rgb[c] * sa + dc * da * (255 - sa) / 255) / oa));
      }
      dst[i * 4 + 3] = static_cast<std::uint8_t>(oa);
    }
  }
}
#endif  // SOAR_WITH_LIBASS

} // namespace

#ifdef SOAR_WITH_LIBASS

struct AssRenderer::Impl {
  // Serializes every libass touch: the decode thread feeds events while
  // the UI thread renders — libass tracks are not internally synchronized.
  std::mutex mutex;
  ASS_Library* library = nullptr;
  ASS_Renderer* renderer = nullptr;
  ASS_Track* track = nullptr;
  AssFrame canvas;
  std::string default_font;
  bool fonts_configured = false;

  ~Impl() {
    if (track) ass_free_track(track);
    if (renderer) ass_renderer_done(renderer);
    if (library) ass_library_done(library);
  }

  bool init() {
    library = ass_library_init();
    if (!library) return false;
    renderer = ass_renderer_init(library);
    if (!renderer) {
      ass_library_done(library);
      library = nullptr;
      return false;
    }
    return true;
  }

  // Font lookup must be configured before rendering (libass renders
  // nothing otherwise). Registered fonts win; the default font file is
  // the fallback. ASS_FONTPROVIDER_NONE keeps this deterministic — no
  // fontconfig, no system font enumeration.
  void configureFonts() {
    if (fonts_configured || !renderer) return;
    ass_set_fonts(renderer, default_font.empty() ? nullptr : default_font.c_str(),
                  "sans-serif", ASS_FONTPROVIDER_NONE, nullptr, 0);
    fonts_configured = true;
  }

  void replaceTrack(ASS_Track* next) {
    if (track) ass_free_track(track);
    track = next;
  }
};

AssRenderer::AssRenderer() : impl_(new Impl) {
  if (!impl_->init()) {
    delete impl_;
    impl_ = nullptr;
  }
}

AssRenderer::~AssRenderer() {
  delete impl_;
}

bool AssRenderer::available() const {
  return impl_ != nullptr;
}

bool AssRenderer::addFont(const char* name, const std::uint8_t* data,
                          std::size_t size) {
  if (!impl_ || !name || !data || size == 0) return false;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  ass_add_font(impl_->library, name,
               reinterpret_cast<const char*>(data),
               static_cast<int>(size));
  // Fonts register into the library; nothing to free here (ass_library_done
  // owns them). Clearing the fonts-configured flag is unnecessary: libass
  // consults library fonts at render time, not configure time.
  return true;
}

void AssRenderer::setDefaultFont(const std::string& path) {
  if (!impl_) return;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->default_font = path;
  // Re-apply: set_fonts must run after the path is known; clear the flag
  // so the next render configures with the new path.
  impl_->fonts_configured = false;
}

bool AssRenderer::loadDocument(const char* data, std::size_t size) {
  if (!impl_ || !data || size == 0) return false;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->configureFonts();
  // ass_read_memory takes ownership semantics over a mutable buffer, so
  // copy (const_cast of the caller's buffer would betray the API).
  std::vector<char> buf(data, data + size);
  ASS_Track* next = ass_read_memory(impl_->library, buf.data(),
                                    static_cast<int>(buf.size()), nullptr);
  if (!next) return false;
  impl_->replaceTrack(next);
  return true;
}

bool AssRenderer::startStream() {
  if (!impl_) return false;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->configureFonts();
  impl_->replaceTrack(ass_new_track(impl_->library));
  return impl_->track != nullptr;
}

bool AssRenderer::feedCodecPrivate(const char* data, std::size_t size) {
  if (!impl_ || !impl_->track || !data || size == 0) return false;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  ass_process_codec_private(impl_->track, data, static_cast<int>(size));
  return true;
}

bool AssRenderer::feedEvent(const char* dialogue_line) {
  if (!impl_ || !impl_->track || !dialogue_line) return false;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const int size = static_cast<int>(std::strlen(dialogue_line));
  if (size == 0) return false;
  ass_process_data(impl_->track, dialogue_line, size);
  return true;
}

void AssRenderer::flushEvents() {
  if (!impl_ || !impl_->track) return;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  ass_flush_events(impl_->track);
}

void AssRenderer::setFrameSize(int w, int h) {
  if (!impl_ || w <= 0 || h <= 0) return;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  // Idempotent: re-passing the live size must not reset the change
  // baseline — the UI calls this every presentation.
  if (impl_->canvas.width == w && impl_->canvas.height == h) return;
  ass_set_frame_size(impl_->renderer, w, h);
  // A new geometry invalidates the previous canvas outright.
  impl_->canvas = AssFrame{};
  impl_->canvas.width = w;
  impl_->canvas.height = h;
}

bool AssRenderer::renderAt(std::int64_t pts_ms, AssFrame* out) {
  if (!impl_ || !out || !impl_->track || !impl_->renderer) {
    if (out) *out = AssFrame{};
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->configureFonts();

  const int w = impl_->canvas.width;
  const int h = impl_->canvas.height;
  if (w <= 0 || h <= 0) {
    *out = AssFrame{};
    return false;
  }

  int detect_change = 0;
  ASS_Image* images =
      ass_render_frame(impl_->renderer, impl_->track,
                       static_cast<long long>(pts_ms), &detect_change);

  if (!images) {
    // Nothing visible at this timestamp. Report the loss exactly once:
    // after clearing, a second empty render has nothing left to lose.
    const bool had_content = !impl_->canvas.rgba.empty();
    impl_->canvas.rgba.clear();
    *out = AssFrame{};
    out->width = w;
    out->height = h;
    out->changed = had_content;  // a vanishing subtitle is a change
    return false;
  }

  // Reuse the canvas buffer; clear to fully transparent first.
  const std::size_t bytes = static_cast<std::size_t>(w) * h * 4;
  if (impl_->canvas.rgba.size() != bytes) {
    impl_->canvas.rgba.assign(bytes, 0);
  } else {
    std::fill(impl_->canvas.rgba.begin(), impl_->canvas.rgba.end(), 0);
  }
  for (const ASS_Image* img = images; img != nullptr; img = img->next) {
    blendGlyph(impl_->canvas.rgba, w, h, img);
  }

  out->rgba = impl_->canvas.rgba;
  out->width = w;
  out->height = h;
  out->changed = detect_change != 0;
  return true;
}

#else // !SOAR_WITH_LIBASS

// Optional-capability stub: same shape, everything inert. Callers gate on
// available() and keep their plain-text path alive.
struct AssRenderer::Impl {};

AssRenderer::AssRenderer() = default;
AssRenderer::~AssRenderer() = default;
bool AssRenderer::available() const { return false; }
bool AssRenderer::addFont(const char*, const std::uint8_t*, std::size_t) { return false; }
void AssRenderer::setDefaultFont(const std::string&) {}
bool AssRenderer::loadDocument(const char*, std::size_t) { return false; }
bool AssRenderer::startStream() { return false; }
bool AssRenderer::feedCodecPrivate(const char*, std::size_t) { return false; }
bool AssRenderer::feedEvent(const char*) { return false; }
void AssRenderer::flushEvents() {}
void AssRenderer::setFrameSize(int, int) {}
bool AssRenderer::renderAt(std::int64_t, AssFrame* out) {
  if (out) *out = AssFrame{};
  return false;
}

#endif // SOAR_WITH_LIBASS

} // namespace soar
