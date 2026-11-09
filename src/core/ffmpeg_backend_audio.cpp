// FFmpegBackend SDL audio output and device selection.
// Split out of ffmpeg_backend.cpp; the backend TUs share
// ffmpeg_backend_internal.h.

#include "soar/core/ffmpeg_backend.h"
#include "ffmpeg_backend_internal.h"

namespace soar {


std::vector<std::string> FFmpegBackend::audioOutputDevices() const {
#ifndef SOAR_WITH_SDL2
  return {};
#else
  // Enumeration needs the audio subsystem but not an open stream; without
  // a usable driver (headless CI) SDL reports zero devices and the caller
  // sees an empty list.
  if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
      return {};
    }
  }
  std::vector<std::string> names;
  const int count = SDL_GetNumAudioDevices(/*iscapture=*/0);
  for (int i = 0; i < count; ++i) {
    const char* name = SDL_GetAudioDeviceName(i, /*iscapture=*/0);
    if (name && *name) {
      names.emplace_back(name);
    }
  }
  return names;
#endif
}


std::string FFmpegBackend::currentAudioOutputDevice() const {
  std::lock_guard<std::mutex> lock(audio_device_mutex_);
  return audio_device_;
}


bool FFmpegBackend::selectAudioOutputDevice(const std::string& name) {
  // A UI-level mistake, not a playback error: lastError() only (the
  // saveScreenshot precedent — no Error event, no state change).
  if (!name.empty()) {
    const std::vector<std::string> known = audioOutputDevices();
    if (std::find(known.begin(), known.end(), name) == known.end()) {
      return fail("selectAudioOutputDevice: unknown device '" + name + "'", /*emit_event=*/false);
    }
  }
  {
    std::lock_guard<std::mutex> lock(audio_device_mutex_);
    audio_device_ = name;
  }
  // Live playback: the decode thread's next ensureOpen() re-opens on the
  // new endpoint (the name is part of its reuse check), so the switch
  // lands within one audio frame. What is still queued on the old device
  // plays out first — that is the shortest, least audible switch, and it
  // costs the UI thread nothing. A switch made while paused applies on
  // resume, for the same reason.
  return true;
}


void FFmpegBackend::playAudioFrame(const AVFrame* frame) {
#ifndef SOAR_WITH_SDL2
  (void)frame;
  return;
#else
  if (!frame) {
    return;
  }

  const int in_rate = frame->sample_rate > 0 ? frame->sample_rate : audio_params_.sample_rate;
  const int in_channels = frame->ch_layout.nb_channels > 0 ? frame->ch_layout.nb_channels : audio_params_.channels;
  if (in_rate <= 0 || in_channels <= 0) {
    return;
  }

  if (!sdl_audio_) {
    sdl_audio_ = std::make_unique<SDLAudio>();
  }

  const double rate = std::clamp(playback_rate_.load(), 0.25, 4.0);
  const int target_rate = std::clamp(static_cast<int>(std::lround(in_rate * rate)), 8000, 192000);

  std::string sdl_err;
  std::string device_name;
  {
    std::lock_guard<std::mutex> lock(audio_device_mutex_);
    device_name = audio_device_;
  }
  if (!sdl_audio_->ensureOpen(target_rate, in_channels, device_name, sdl_err)) {
    return;
  }

  const AVSampleFormat out_fmt = AV_SAMPLE_FMT_FLT;
  const int out_rate = sdl_audio_->sample_rate;
  const int out_channels = sdl_audio_->channels;

  const AVSampleFormat in_fmt = static_cast<AVSampleFormat>(frame->format);

  std::uint64_t key = static_cast<std::uint64_t>(in_rate);
  key = (key * 1315423911u) ^ static_cast<std::uint64_t>(in_channels);
  key = (key * 1315423911u) ^ static_cast<std::uint64_t>(in_fmt);
  key = (key * 1315423911u) ^ static_cast<std::uint64_t>(out_rate);
  key = (key * 1315423911u) ^ static_cast<std::uint64_t>(out_channels);

  if (audio_resampler_ && audio_resample_key_ != key) {
    swr_free(&audio_resampler_);
    audio_resampler_ = nullptr;
    audio_resample_key_ = 0;
  }

  if (!audio_resampler_) {
    AVChannelLayout out_layout{};
    av_channel_layout_default(&out_layout, out_channels);

    AVChannelLayout in_layout{};
    if (frame->ch_layout.nb_channels > 0 && av_channel_layout_copy(&in_layout, &frame->ch_layout) >= 0) {
      // ok
    } else {
      av_channel_layout_default(&in_layout, in_channels);
    }

    SwrContext* swr = nullptr;
    if (swr_alloc_set_opts2(
          &swr,
          &out_layout,
          out_fmt,
          out_rate,
          &in_layout,
          in_fmt,
          in_rate,
          0,
          nullptr
        ) < 0) {
      av_channel_layout_uninit(&in_layout);
      av_channel_layout_uninit(&out_layout);
      return;
    }
    av_channel_layout_uninit(&in_layout);
    av_channel_layout_uninit(&out_layout);

    if (swr_init(swr) < 0) {
      swr_free(&swr);
      return;
    }
    audio_resampler_ = swr;
    audio_resample_key_ = key;
  }

  const int64_t delay = swr_get_delay(audio_resampler_, in_rate);
  const int out_samples = static_cast<int>(
    av_rescale_rnd(delay + frame->nb_samples, out_rate, in_rate, AV_ROUND_UP)
  );
  if (out_samples <= 0) {
    return;
  }

  std::vector<float> out_data(static_cast<std::size_t>(out_samples * out_channels));
  uint8_t* out_planes[1] = { reinterpret_cast<uint8_t*>(out_data.data()) };
  const uint8_t** in_planes = const_cast<const uint8_t**>(frame->extended_data);

  const int converted = swr_convert(audio_resampler_, out_planes, out_samples, in_planes, frame->nb_samples);
  if (converted <= 0) {
    return;
  }

  const std::size_t frames = static_cast<std::size_t>(converted);
  const double volume = muted_.load() ? 0.0 : std::clamp(volume_.load(), 0.0, 1.0);
  if (volume != 1.0) {
    for (std::size_t i = 0; i < frames * static_cast<std::size_t>(out_channels); ++i) {
      out_data[i] = static_cast<float>(out_data[i] * volume);
    }
  }

  // Backpressure: cap queued audio to ~500ms. The loop bails on a stop or a
  // pending seek: without the check its only exit was the device draining the
  // queue, so a device that stopped draining would park the decode thread in
  // SDL_Delay and stop()'s join would hang with it (BUGS.md #4).
  const std::size_t bytes_per_second =
    static_cast<std::size_t>(out_rate) * static_cast<std::size_t>(out_channels) * sizeof(float);
  const std::size_t max_queued = bytes_per_second / 2;
  while (sdl_audio_->queuedBytes() > max_queued &&
         !should_stop_decoding_ && !seek_requested_.load()) {
    SDL_Delay(5);
  }
  if (sdl_audio_->queuedBytes() > max_queued) {
    // A stop or seek won the race; the backlog is the audio for the position
    // we are leaving, so drop this frame instead of adding to it (the seek
    // clears the device queue, the stop drains nothing).
    return;
  }

  const std::size_t out_bytes = frames * static_cast<std::size_t>(out_channels) * sizeof(float);
  (void)sdl_audio_->queue(out_data.data(), out_bytes, sdl_err);
#endif
}


//=============================================================================
// Runtime audio track switching
//=============================================================================

bool FFmpegBackend::audioTrackPending() {
  std::lock_guard<std::mutex> lock(pending_audio_mutex_);
  return pending_audio_track_ >= 0;
}


void FFmpegBackend::applyPendingAudioTrack() {
  AVCodecContext* new_decoder = nullptr;
  int new_track = -1;
  {
    std::lock_guard<std::mutex> lock(pending_audio_mutex_);
    if (pending_audio_track_ < 0) {
      return;
    }
    new_decoder = pending_audio_decoder_;
    new_track = pending_audio_track_;
    pending_audio_decoder_ = nullptr;
    pending_audio_track_ = -1;
  }

  if (!new_decoder) {
    return;
  }

  // Resume at the position playback had reached when the switch was
  // requested. This runs on the decode thread, which never takes
  // decode_mutex_ (close() joins while holding it), so seekToTimestamp
  // executes on its lock-free path by design.
  std::chrono::milliseconds resume_at{0};
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    resume_at = current_position_;
  }

  if (!seekToTimestamp(resume_at, /*emit_event=*/false)) {
    // Keep the current track and decoder; drop the new one.
    avcodec_free_context(&new_decoder);
    {
      std::lock_guard<std::mutex> lock(info_mutex_);
      media_info_.selected_audio = audio_stream_index_;
    }
    fail(fmt::format(
      "selectTrack: failed to seek new audio track {} to the resume position", new_track));
    return;
  }

  avcodec_free_context(&audio_decoder_);
  audio_decoder_ = new_decoder;
  audio_stream_index_ = new_track;
  audio_params_.sample_rate = audio_decoder_->sample_rate;
  audio_params_.channels = audio_decoder_->ch_layout.nb_channels;
  audio_params_.channel_layout = audio_decoder_->ch_layout.u.mask;

  {
    std::lock_guard<std::mutex> lock(info_mutex_);
    media_info_.selected_audio = new_track;
  }
  emit(Event{EventType::MediaInfoChanged});
}

} // namespace soar
