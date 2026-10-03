// Audio extraction: decode a media source's audio track to a 16 kHz mono
// 16-bit PCM WAV file. First slice of the §6 "语音实时翻译" feature — ASR
// (whisper-class) and the subtitle pipeline consume exactly this shape, so
// the extractor owns the libavformat/libavcodec/libswresample plumbing and
// keeps it out of the backend (the backend plays; this component pulls).
//
// Only the first/best audio track is taken. Output is always pcm_s16le at
// 16000 Hz mono regardless of source — the ASR wire format — so callers do
// not deal with sample-rate/channel negotiation.
//
// Thread-safety: a single call runs to completion on the caller's thread;
// no internal state, safe to call again after it returns (the ffmpeg
// contexts are created and freed per call).

#ifndef SOAR_CORE_AUDIO_EXTRACT_H_
#define SOAR_CORE_AUDIO_EXTRACT_H_

#include <string>

namespace soar {

// Decodes the best audio stream of `uri` (local path or http(s)://) and
// writes a 16 kHz mono s16le WAV to `out_wav`. Returns true on success;
// on failure returns false and (when non-null) sets *err to a human
// reason. Overwrites out_wav. Sources with no audio track fail with an
// explicit message rather than producing an empty file.
bool extractAudioToWav(const std::string& uri, const std::string& out_wav,
                       std::string* err);

}  // namespace soar

#endif  // SOAR_CORE_AUDIO_EXTRACT_H_
