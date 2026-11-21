#!/usr/bin/env bash
# Config-toggle effect pipeline (todo DOC-GIF): for every switch whose state
# changes what the user sees, capture an off still, record the transition,
# capture an on still, and encode a palette GIF — into docs/public/config-effects/.
# Re-runnable: any config change regenerates the whole set the same way
# (same fixtures, same keys), which doubles as a config-effect regression check.
#
# Prereqs: build-system/soar (IMGUI=ON), scripts/generate_test_media.sh once,
#          ffmpeg with x11grab, imagemagick import, xdotool.
# Usage:   scripts/gen_config_gifs.sh [toggle ...]   (default: all)
set -u -o pipefail
cd "$(dirname "$0")/.." || exit 1

APP=build-system/soar
TM=build-system/testmedia
OUT=docs/public/config-effects
DISP=:115
STATE=/tmp/soar-gif-state
REC=/tmp/soar-gif-rec.mp4

mkdir -p "$OUT" "$STATE/soar" 2>/dev/null
: > "$STATE/soar/library.txt"

export SOAR_UI_BITMAP_FONT=1 SDL_AUDIODRIVER=dummy XDG_STATE_HOME="$STATE"

# ---- one Xvfb for the whole run; the app window is 960x540 centered -------
pkill -f "Xvfb $DISP" 2>/dev/null; sleep 0.5
Xvfb "$DISP" -screen 0 1280x800x24 -ac -nolisten tcp >/dev/null 2>&1 &
XVFB=$!
sleep 1
kill -0 "$XVFB" 2>/dev/null || { echo "xvfb failed"; exit 1; }
export DISPLAY="$DISP"
RECPID=""
trap 'kill "$XVFB" 2>/dev/null; pkill -x soar 2>/dev/null; [ -n "$RECPID" ] && kill "$RECPID" 2>/dev/null' EXIT

shoot() {  # still of the app window only (960x540 at 160,130), so every on/off
  # pair is the same canvas as the GIF (the window is SDL-centered on 1280x800)
  import -window root -crop 960x540+160+130 +repage "$OUT/$1.png" 2>/dev/null; }
key()   { xdotool key "$1"; }
click() { xdotool mousemove "$1" "$2"; sleep 0.25; xdotool click 1; }
rec_start() {  # record the app window area (960x540 centered on 1280x800)
  ffmpeg -hide_banner -loglevel error -f x11grab -framerate 10 -video_size 960x540 \
    -i "$DISP+160+130" -pix_fmt yuv420p -y "$REC" 2>/dev/null &
  RECPID=$!
}
rec_stop_gif() {  # stop the recorder and encode a palette GIF at a web size
  rec_stop
  rec_encode "$1"
}

rec_stop() {  # stop the recorder only, leaving the take for frame extraction
  sleep 0.4; kill -INT "$RECPID" 2>/dev/null; wait "$RECPID" 2>/dev/null; RECPID=""
}

rec_encode() {  # encode the recorded take into a looping palette GIF
  ffmpeg -hide_banner -loglevel error -i "$REC" -vf \
    "fps=8,scale=480:-1:flags=lanczos,split[s0][s1];[s0]palettegen=max_colors=63[p];[s1][p]paletteuse=dither=bayer:bayer_scale=4" \
    -loop 0 -y "$OUT/$1.gif" 2>/dev/null
}

still_from_take() {  # still_from_take <name> <first|last>: cut the still out of
  # the take so the on/off pair comes from the same recorded window rather than
  # from live captures that drift out of step with the media clock.
  local name=$1 which=$2
  if [ "$which" = last ]; then
    ffmpeg -hide_banner -loglevel error -sseof -0.12 -i "$REC" \
      -frames:v 1 -y "$OUT/${name}_off.png" 2>/dev/null
  else
    ffmpeg -hide_banner -loglevel error -i "$REC" \
      -frames:v 1 -y "$OUT/${name}_on.png" 2>/dev/null
  fi
}

launch() {  # launch <theme> <file...>
  echo "$1" > "$STATE/soar/theme.txt"
  shift
  pkill -x soar 2>/dev/null; sleep 0.6
  "$APP" "$@" >/tmp/soar-gif-app.log 2>&1 &
  sleep 3
}

launch_no_shortcuts() {  # launch with --no-shortcuts, same args as launch
  pkill -x soar 2>/dev/null; sleep 0.6
  "$APP" --no-shortcuts "$TM/subs_media.mkv" >/tmp/soar-gif-app.log 2>&1 &
  sleep 3
}

launch_fast() {  # launch and pause on the OSC's round play button (window
  # 44,491 — probed off the orange face's bbox): a mouse click works even with
  # --no-shortcuts, so both runs of a launch-flag pair sit on almost the same
  # frame and the off/on pair differs in the control (the chip) rather than in
  # the scrolling test pattern.
  echo dark > "$STATE/soar/theme.txt"
  pkill -x soar 2>/dev/null; sleep 0.6
  "$APP" "$@" >/tmp/soar-gif-app.log 2>&1 &
  sleep 2.2
  click 204 621; sleep 0.5
}

launch_fast_no_shortcuts() {
  pkill -x soar 2>/dev/null; sleep 0.6
  "$APP" --no-shortcuts "$TM/subs_media.mkv" >/tmp/soar-gif-app.log 2>&1 &
  sleep 2.2
  click 204 621; sleep 0.5
}

# ============================ toggles ============================

gif_theme() {  # Appearance: dark -> light (the y key)
  launch dark "$TM/subs_media.mkv"
  key space; sleep 0.6                       # pause so the frame is stable
  shoot theme_off
  rec_start; sleep 0.5; key y; sleep 1.6; rec_stop_gif theme
  sleep 0.3; shoot theme_on
  pkill -x soar; sleep 0.6
}

gif_subtitles() {  # Subtitle visibility (V): subs shown -> hidden
  # The fixture's srt does not auto-select (media-info logs sub=-1), and the
  # subtitle text renders only on active playback frames (paused frames early-out
  # the renderer), so the flow is: pick the track from the OSC subtitle combo
  # while the launch OSC is still up, then press V while the position sits inside
  # the "Two Lines" cue (4-6s media time). The media is slowed to 0.5x (, twice)
  # so that cue spans ~4 real seconds, which leaves room for both stills and the
  # recording without the clip ending (its 6s timeline is otherwise too short).
  launch dark "$TM/subs_media.mkv"
  key comma; sleep 0.3; key comma; sleep 0.3   # 1.0x -> 0.75x -> 0.5x
  click 677 625                            # OSC Sub combo (window 517,495)
  sleep 0.3; click 674 559                 # track row (window 514,429)
  for _ in $(seq 1 80); do                 # poll the log: wait for a cue frame
    P=$(grep -oE "position=[0-9]+ms" /tmp/soar-gif-app.log | tail -1 | grep -oE "[0-9]+")
    [ -n "$P" ] && [ "$P" -ge 4000 ] && [ "$P" -le 5200 ] && break
    sleep 0.1
  done
  rec_start                                # record the V transition
  sleep 0.4; key v; sleep 1.3
  rec_stop
  still_from_take subtitles first          # "Two Lines" visible on the frame
  still_from_take subtitles last           # caption gone + "Subtitles off" toast
  rec_encode subtitles
  pkill -x soar; sleep 0.6
}

gif_mute() {  # Mute (M): muted -> unmuted, the OSC volume chip flips
  launch dark "$TM/subs_media.mkv"
  key space; sleep 0.4; key m; sleep 0.4     # start muted = the off state
  shoot mute_off
  rec_start; sleep 0.5; key m; sleep 1.2; rec_stop_gif mute
  sleep 0.3; shoot mute_on
  pkill -x soar; sleep 0.6
}

gif_rate() {  # Playback speed (period): 1.0x -> 1.2x, toast + rate chip
  launch dark "$TM/subs_media.mkv"
  key space; sleep 0.4
  shoot rate_off
  rec_start; sleep 0.5; key period; sleep 1.4; rec_stop_gif rate
  sleep 0.3; shoot rate_on
  pkill -x soar; sleep 0.6
}

gif_fullscreen() {  # Fullscreen (F): windowed -> fullscreen
  launch dark "$TM/subs_media.mkv"
  key space; sleep 0.4
  shoot fullscreen_off
  rec_start; sleep 0.5; key f; sleep 1.8; rec_stop_gif fullscreen
  sleep 0.3; shoot fullscreen_on
  key Escape; sleep 0.6                       # fullscreen ESC = leave fullscreen
  pkill -x soar; sleep 0.6
}

# gif_switch <x> <y> <name> [nav]: click a settings switch and record the flip.
# The click pins are the walkthrough's window-relative calibration
# (tests/test_cli.cpp:505-547), each +160/+130 for the centered window. nav
# selects the settings tab: Settings opens on General, so the empty value needs
# no nav click.
gif_switch() {
  local x=$1 y=$2 name=$3 nav=${4:-}
  launch dark "$TM/subs_media.mkv"
  key space; sleep 0.4; key g; sleep 1.0      # Settings opens on General
  case "$nav" in
    playback) click 258 238;;                # nav: Playback (window 98,108)
    subtitles) click 258 268;;                # nav: Subtitles (window 98,138)
    network) click 258 320;;                  # nav: Network (window 98,190)
  esac
  sleep 0.6
  shoot "${name}_off"
  rec_start; sleep 0.5
  click "$x" "$y"; sleep 1.2
  rec_stop_gif "$name"
  sleep 0.3; shoot "${name}_on"
  key g; sleep 0.3
  pkill -x soar; sleep 0.6
}

gif_cli_headless() {  # --headless: windowed run vs terminal-only run
  launch dark "$TM/subs_media.mkv"; key space; sleep 0.4
  shoot cli_headless_off
  pkill -x soar; sleep 0.6
  # on: no window at all — the honest still is the bare desktop plus the
  # headless trail; the GIF shows the window disappearing, then nothing.
  "$APP" --headless "$TM/subs_media.mkv" >/tmp/soar-headless.log 2>&1
  shoot cli_headless_on
  launch dark "$TM/subs_media.mkv"; key space; sleep 0.4
  rec_start; sleep 0.5; pkill -x soar; sleep 0.8
  "$APP" --headless "$TM/subs_media.mkv" >/dev/null 2>&1
  sleep 1.5; rec_stop_gif cli_headless
}

gif_cli_no_shortcuts() {  # --no-shortcuts: the same M press that mutes a normal
  # run (mute.gif) leaves the chip alone. Both stills are paused at ~1s so the
  # frame behind the controls nearly matches; the pair is a two-run comparison.
  launch_fast "$TM/subs_media.mkv"; key m; sleep 0.6
  shoot cli_no_shortcuts_off                # normal run: M mutes
  pkill -x soar; sleep 0.6
  launch_fast_no_shortcuts
  rec_start; sleep 0.5; key m; sleep 1.2
  rec_stop_gif cli_no_shortcuts
  sleep 0.3; shoot cli_no_shortcuts_on       # --no-shortcuts: M does nothing
  pkill -x soar; sleep 0.6
}

gif_switch_remember()      { gif_switch 1068 247 switch_remember; }          # window 908,117
gif_switch_esc()           { gif_switch 1068 306 switch_esc; }               # window 908,176
gif_switch_shuffle()       { gif_switch 1068 365 switch_shuffle playback; }  # window 908,235
gif_switch_subs_show()     { gif_switch 706 247 switch_subs_show subtitles; }  # window 546,117
gif_switch_subs_autoshow() { gif_switch 706 306 switch_subs_autoshow subtitles; }  # 546,176
gif_switch_subs_autodl()   { gif_switch 1064 466 switch_subs_autodl subtitles; }  # 904,336

ALL="theme subtitles mute rate fullscreen switch_esc switch_remember switch_shuffle switch_subs_show switch_subs_autoshow switch_subs_autodl cli_headless cli_no_shortcuts"
case "${1:-all}" in
  theme) gif_theme ;;
  subtitles) gif_subtitles ;;
  mute) gif_mute ;;
  rate) gif_rate ;;
  fullscreen) gif_fullscreen ;;
  switch_esc) gif_switch_esc ;;
  switch_remember) gif_switch_remember ;;
  switch_shuffle) gif_switch_shuffle ;;
  switch_subs_show) gif_switch_subs_show ;;
  switch_subs_autoshow) gif_switch_subs_autoshow ;;
  switch_subs_autodl) gif_switch_subs_autodl ;;
  cli_headless) gif_cli_headless ;;
  cli_no_shortcuts) gif_cli_no_shortcuts ;;
  all) for t in $ALL; do echo "== $t =="; "gif_$t" || echo "$t FAILED"; done ;;
  *) echo "unknown toggle: $1 (one of: $ALL)"; exit 2 ;;
esac
verify() {  # regression check: every on/off pair must still differ (a toggle
  # that stopped doing anything would produce two identical frames)
  local fail=0
  for n in $ALL; do
    local a="$OUT/${n}_on.png" b="$OUT/${n}_off.png" g="$OUT/$n.gif"
    [ -f "$a" ] && [ -f "$b" ] && [ -f "$g" ] || { echo "$n: missing files"; fail=1; continue; }
    local px
    # compare prints "<pixels> (<pct>)" on stderr; keep only the pixel count
    px=$(compare -metric AE "$a" "$b" null: 2>&1 | grep -oE '^[0-9]+' | head -1)
    [ -n "$px" ] && [ "$px" -gt 0 ] || { echo "$n: on/off identical!"; fail=1; }
  done
  [ "$fail" -eq 0 ] && echo "verify: all $(echo "$ALL" | wc -w) toggles have distinct on/off pairs"
  return "$fail"
}

echo "outputs in $OUT:"
ls -la "$OUT" | grep -E "\.(gif|png)$"
verify
