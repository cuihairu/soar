// extract_audio -- Dev tool: decode a media source's audio track to a
// 16 kHz mono WAV. Slice of the §6 "语音实时翻译" ASR path; not shipped.
//
// Usage: extract_audio <uri> <out.wav>

#include "soar/core/audio_extract.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "Usage: %s <uri> <out.wav>\n", argv[0]);
    return 2;
  }
  std::string err;
  if (!soar::extractAudioToWav(argv[1], argv[2], &err)) {
    std::fprintf(stderr, "extract_audio: %s\n", err.c_str());
    return 1;
  }
  std::fprintf(stderr, "extract_audio: wrote %s\n", argv[2]);
  return 0;
}
