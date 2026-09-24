#!/usr/bin/env bash
# Generates the synthetic media fixtures the FFmpeg backend tests need.
# Everything is a fast lavfi synthesis: no network, no real assets, no
# external samples. The script prints KEY=VALUE lines (one per fixture) so
# a caller can pipe them into GitHub Actions' $GITHUB_ENV; locally they can
# be sourced or exported by hand before running ctest.
set -euo pipefail

out_dir="${1:-build-system/testmedia}"
mkdir -p "$out_dir"
case "$out_dir" in
  /*) media_dir="$out_dir" ;;
  *)  media_dir="$PWD/$out_dir" ;;
esac

# Dual audio: the workhorse for track switching and lifecycle tests.
# The audio streams carry titles so the track enumeration exercises the
# metadata name instead of the codec-based fallback (the video stream
# has no title and covers that fallback). The second stream runs at a
# different sample rate, so switching to it forces the SDL audio device
# to reopen with new parameters instead of reusing the open one.
ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i testsrc=size=160x120:rate=15 \
  -f lavfi -i sine=frequency=440 \
  -f lavfi -i sine=frequency=880:sample_rate=8000 \
  -map 0:v -map 1:a -map 2:a -t 6 \
  -metadata:s:a:0 title="Sine 440" \
  -metadata:s:a:1 title="Sine 880" \
  -c:v ffvhuff -c:a pcm_s16le \
  "$media_dir/sample_dual_audio.mkv"

# Audio only: position must be driven by audio frames, and seeks resolve
# through the audio stream (there is no video stream index).
ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i sine=frequency=440 \
  -t 6 -c:a pcm_s16le \
  "$media_dir/audio_only.mkv"

# Sidecar subtitle text. It is used both as a mapped subtitle stream and
# (as a separate copy) as a container attachment, which must be ignored by
# track enumeration.
printf '1\n00:00:00,000 --> 00:00:02,000\nHello\n\n2\n00:00:02,000 --> 00:00:04,000\nWorld\n' \
  > "$media_dir/subs.srt"

ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i testsrc=size=160x120:rate=15 \
  -f lavfi -i sine=frequency=440 \
  -i "$media_dir/subs.srt" \
  -attach "$media_dir/subs.srt" \
  -map 0:v -map 1:a -map 2:s \
  -c:v ffvhuff -c:a pcm_s16le -c:s srt \
  -metadata:s:s:0 language=eng \
  -metadata:s:t mimetype=text/plain \
  -t 4 \
  "$media_dir/subs_media.mkv"

# Subtitle-only container: opens fine but has no audio or video stream,
# so the backend's open must fail with the "no video or audio stream"
# fatal instead of handing out a bogus MediaInfo.
ffmpeg -hide_banner -loglevel error -y \
  -i "$media_dir/subs.srt" -c:s srt \
  "$media_dir/subs_only.mkv"

# Mid-stream resolution and pixel-format change: two h264 segments
# concatenated at the elementary-stream level. The first segment is
# yuv422p, so its frames go through the video converter; the second is
# yuv420p (and larger), which bypasses the converter entirely - so the
# run covers both the rebuild on a parameter change and the pass-through
# path after it.
ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i testsrc=size=160x120:rate=15 -t 2 \
  -c:v libx264 -pix_fmt yuv422p "$media_dir/seg_a.ts"
ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i testsrc2=size=320x240:rate=15 -t 2 \
  -c:v libx264 -pix_fmt yuv420p "$media_dir/seg_b.ts"
cat "$media_dir/seg_a.ts" "$media_dir/seg_b.ts" > "$media_dir/multi_res.ts"
rm -f "$media_dir/seg_a.ts" "$media_dir/seg_b.ts"

# Corrupt-payload fixtures, built by patching the Matroska CodecID of a
# healthy h264+audio file in place. The replacement is equal-length, so
# the EBML structure is untouched and the file still opens normally:
# - "V_MPEG4/ISO/ASP" claims MPEG-4 part 2 while the packets stay h264,
#   so the decoder opens fine and then rejects every frame - the decode
#   loop must surface that as a fatal error instead of spinning.
# - "V_MPEG4/ISO/XXX" is a codec id FFmpeg has no decoder for, so the
#   open path must fail with the "codec not found" fatal before any
#   decoding starts.
# python3 does the byte patch (ffmpeg cannot write an unknown codec id
# itself); it is preinstalled on every machine that runs this script.
ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i testsrc=size=160x120:rate=15 \
  -f lavfi -i sine=frequency=440 \
  -map 0:v -map 1:a -t 3 \
  -c:v libx264 -pix_fmt yuv420p -c:a aac \
  "$media_dir/corrupt_base.mkv"
python3 - "$media_dir" <<'PYEOF'
import sys

media_dir = sys.argv[1]
with open(f"{media_dir}/corrupt_base.mkv", "rb") as f:
    data = f.read()
assert data.count(b"V_MPEG4/ISO/AVC") == 1, "h264 CodecID not found exactly once"
with open(f"{media_dir}/corrupt_decode.mkv", "wb") as f:
    f.write(data.replace(b"V_MPEG4/ISO/AVC", b"V_MPEG4/ISO/ASP"))
with open(f"{media_dir}/unknown_codec.mkv", "wb") as f:
    f.write(data.replace(b"V_MPEG4/ISO/AVC", b"V_MPEG4/ISO/XXX"))
PYEOF
rm -f "$media_dir/corrupt_base.mkv"

# Truncated containers for the open-path failure contract: 64 bytes does
# not even contain the Matroska segment header, and 512 bytes still cuts
# inside the header region. Both are rejected at the demuxer's own open
# on FFmpeg 6.1 (CI); FFmpeg 8 accepts the 512-byte open and only fails
# at find_stream_info, so the tests assert the shared contract (no open,
# Error state) rather than which stage refused the file.
head -c 64 "$media_dir/sample_dual_audio.mkv" > "$media_dir/trunc_tiny.mkv"
head -c 512 "$media_dir/sample_dual_audio.mkv" > "$media_dir/trunc_mid.mkv"

echo "SOAR_TEST_MEDIA=$media_dir/sample_dual_audio.mkv"
echo "SOAR_TEST_AUDIO_ONLY=$media_dir/audio_only.mkv"
echo "SOAR_TEST_SUBS_MEDIA=$media_dir/subs_media.mkv"
echo "SOAR_TEST_SUBS_ONLY=$media_dir/subs_only.mkv"
echo "SOAR_TEST_MULTI_RES=$media_dir/multi_res.ts"
echo "SOAR_TEST_CORRUPT_DECODE=$media_dir/corrupt_decode.mkv"
echo "SOAR_TEST_UNKNOWN_CODEC=$media_dir/unknown_codec.mkv"
echo "SOAR_TEST_TRUNCATED_TINY=$media_dir/trunc_tiny.mkv"
echo "SOAR_TEST_TRUNCATED_MID=$media_dir/trunc_mid.mkv"
