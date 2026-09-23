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
ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i testsrc=size=160x120:rate=15 \
  -f lavfi -i sine=frequency=440 \
  -f lavfi -i sine=frequency=880 \
  -map 0:v -map 1:a -map 2:a -t 6 \
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

# Mid-stream resolution change: two h264 segments concatenated at the
# elementary-stream level, so the decoder hands out frames of a new size
# halfway through and the video converter must rebuild itself. Both
# segments are yuv422p so every frame goes through the converter (420p
# frames bypass it entirely) while the chroma format stays constant.
ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i testsrc=size=160x120:rate=15 -t 2 \
  -c:v libx264 -pix_fmt yuv422p "$media_dir/seg_a.ts"
ffmpeg -hide_banner -loglevel error -y \
  -f lavfi -i testsrc2=size=320x240:rate=15 -t 2 \
  -c:v libx264 -pix_fmt yuv422p "$media_dir/seg_b.ts"
cat "$media_dir/seg_a.ts" "$media_dir/seg_b.ts" > "$media_dir/multi_res.ts"
rm -f "$media_dir/seg_a.ts" "$media_dir/seg_b.ts"

echo "SOAR_TEST_MEDIA=$media_dir/sample_dual_audio.mkv"
echo "SOAR_TEST_AUDIO_ONLY=$media_dir/audio_only.mkv"
echo "SOAR_TEST_SUBS_MEDIA=$media_dir/subs_media.mkv"
echo "SOAR_TEST_SUBS_ONLY=$media_dir/subs_only.mkv"
echo "SOAR_TEST_MULTI_RES=$media_dir/multi_res.ts"
