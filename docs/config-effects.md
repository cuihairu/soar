# Config effects

> Every user-visible on/off switch in the player, with its **off frame**, its **on frame**,
> and the **transition** animated. This page is generated evidence, not a manual: each
> frame was captured by driving the shipped binary on an exclusive Xvfb display.

| | |
| --- | --- |
| **Fixture** | `scripts/generate_test_media.sh` media: a 160×120 `testsrc` pattern plus a sine tone plus a three-cue srt (`Hello` / `World` / `Two Lines`). The large digit on the frame is the pattern's own counter and is part of the video, not a caption. |
| **Build under test** | `build-system/soar` (IMGUI on), `SOAR_UI_BITMAP_FONT=1` — the pinned walkthrough configuration |
| **Window** | 960×540, SDL-centred on a 1280×800 `Xvfb` display; stills are cropped to the window |
| **Pipeline** | `scripts/gen_config_gifs.sh [toggle \| all]` — re-runnable, so any config change regenerates the whole set the same way. It doubles as a config-effect regression check: each on/off pair must still differ. |

## Inventory

| # | Toggle | Where | Off | On | Transition |
| --- | --- | --- | --- | --- | --- |
| 1 | Appearance (dark ↔ light) | `y`, Settings → Appearance | [frame](/config-effects/theme_off.png) | [frame](/config-effects/theme_on.png) | [gif](/config-effects/theme.gif) |
| 2 | Subtitle visibility | `v`, OSC Sub combo | [frame](/config-effects/subtitles_off.png) | [frame](/config-effects/subtitles_on.png) | [gif](/config-effects/subtitles.gif) |
| 3 | Mute | `m`, OSC | [frame](/config-effects/mute_off.png) | [frame](/config-effects/mute_on.png) | [gif](/config-effects/mute.gif) |
| 4 | Playback rate | `.`, `,`, Settings → Playback | [frame](/config-effects/rate_off.png) | [frame](/config-effects/rate_on.png) | [gif](/config-effects/rate.gif) |
| 5 | Fullscreen | `f` | [frame](/config-effects/fullscreen_off.png) | [frame](/config-effects/fullscreen_on.png) | [gif](/config-effects/fullscreen.gif) |
| 6 | Remember last media | Settings → General | [frame](/config-effects/switch_remember_off.png) | [frame](/config-effects/switch_remember_on.png) | [gif](/config-effects/switch_remember.gif) |
| 7 | ESC exits fullscreen | Settings → General | [frame](/config-effects/switch_esc_off.png) | [frame](/config-effects/switch_esc_on.png) | [gif](/config-effects/switch_esc.gif) |
| 8 | Shuffle queue | Settings → Playback | [frame](/config-effects/switch_shuffle_off.png) | [frame](/config-effects/switch_shuffle_on.png) | [gif](/config-effects/switch_shuffle.gif) |
| 9 | Show subtitles | Settings → Subtitles | [frame](/config-effects/switch_subs_show_off.png) | [frame](/config-effects/switch_subs_show_on.png) | [gif](/config-effects/switch_subs_show.gif) |
| 10 | Auto-show subtitles | Settings → Subtitles | [frame](/config-effects/switch_subs_autoshow_off.png) | [frame](/config-effects/switch_subs_autoshow_on.png) | [gif](/config-effects/switch_subs_autoshow.gif) |
| 11 | Auto-download subtitles | Settings → Subtitles | [frame](/config-effects/switch_subs_autodl_off.png) | [frame](/config-effects/switch_subs_autodl_on.png) | [gif](/config-effects/switch_subs_autodl.gif) |
| 12 | `--headless` | CLI flag | [frame](/config-effects/cli_headless_off.png) | [frame](/config-effects/cli_headless_on.png) | [gif](/config-effects/cli_headless.gif) |
| 13 | `--no-shortcuts` | CLI flag | [frame](/config-effects/cli_no_shortcuts_off.png) | [frame](/config-effects/cli_no_shortcuts_on.png) | [gif](/config-effects/cli_no_shortcuts.gif) |

Flags with a visible effect that this pipeline does not capture are listed at the
[bottom](#flags-without-a-gif) with the reason.

## Appearance

`y` toggles the theme; Settings → Appearance does the same from the page. The shell
(status chips, OSC, settings surface) recolours; the video canvas and the captions on
it are content and stay dark in both themes.

| Off (dark) | On (light) | Transition |
| --- | --- | --- |
| ![](/config-effects/theme_off.png) | ![](/config-effects/theme_on.png) | ![](/config-effects/theme.gif) |

## Subtitle visibility

`v` toggles captions for the selected subtitle track. Two fixture behaviours are worth
knowing before reading the frames:

- the srt in the fixture **does not auto-select** — the open event logs
  `selected(video=0,audio=1,sub=-1)` — so the pipeline picks the track from the OSC
  subtitle combo first;
- captions are drawn only on **active playback frames**. A paused frame early-outs the
  renderer, which is why the pipeline waits for a cue while playing rather than
  pausing on one.

The pipeline slows the media to 0.5× (`,`, twice) so the four-second cue
(`Two Lines`, 4–6 s of media time) lasts long enough to hold both stills inside it
without the six-second clip ending.

| Off (caption hidden) | On (caption drawn) | Transition |
| --- | --- | --- |
| ![](/config-effects/subtitles_off.png) | ![](/config-effects/subtitles_on.png) | ![](/config-effects/subtitles.gif) |

## Mute

`m` flips mute. The chip reads `Unmute` while muted and the volume rail drops to zero.

| Off (unmuted) | On (muted) | Transition |
| --- | --- | --- |
| ![](/config-effects/mute_off.png) | ![](/config-effects/mute_on.png) | ![](/config-effects/mute.gif) |

## Playback rate

`.`, `,` step the rate through six values (0.5×, 0.75×, 1.0×, 1.25×, 1.5×, 2.0×);
Settings → Playback cycles the same list. The chip here goes 1× → 1.25×; the trailing
`x` is clipped by the combo width — the same cosmetic the 2026-11-21 UI audit logged as
a P3.

| Off (1×) | On (1.25×) | Transition |
| --- | --- | --- |
| ![](/config-effects/rate_off.png) | ![](/config-effects/rate_on.png) | ![](/config-effects/rate.gif) |

## Fullscreen

`f` enters fullscreen; `Escape` in fullscreen returns to the window (or quits when the
ESC-exits-fullscreen switch is off).

| Off (windowed) | On (fullscreen) | Transition |
| --- | --- | --- |
| ![](/config-effects/fullscreen_off.png) | ![](/config-effects/fullscreen_on.png) | ![](/config-effects/fullscreen.gif) |

## Settings → General

Both General switches change behaviour rather than the frame, so the visible effect is
the pill itself.

**Remember last media** — resuming the last playback position at launch.

| Off | On | Transition |
| --- | --- | --- |
| ![](/config-effects/switch_remember_off.png) | ![](/config-effects/switch_remember_on.png) | ![](/config-effects/switch_remember.gif) |

**ESC exits fullscreen** — copy on the row states what the shortcut actually does: in
fullscreen ESC returns to the window; otherwise ESC quits.

| Off | On | Transition |
| --- | --- | --- |
| ![](/config-effects/switch_esc_off.png) | ![](/config-effects/switch_esc_on.png) | ![](/config-effects/switch_esc.gif) |

## Settings → Playback

**Shuffle queue** — reorders the queue instead of playing it in order.

| Off | On | Transition |
| --- | --- | --- |
| ![](/config-effects/switch_shuffle_off.png) | ![](/config-effects/switch_shuffle_on.png) | ![](/config-effects/switch_shuffle.gif) |

## Settings → Subtitles

Three switches, and the same visibility state as `v` in the second row.

**Show subtitles** — the renderer early-outs when this is off.

| Off | On | Transition |
| --- | --- | --- |
| ![](/config-effects/switch_subs_show_off.png) | ![](/config-effects/switch_subs_show_on.png) | ![](/config-effects/switch_subs_show.gif) |

**Auto-show subtitles** — selects and shows a subtitle track when playback opens.

| Off | On | Transition |
| --- | --- | --- |
| ![](/config-effects/switch_subs_autoshow_off.png) | ![](/config-effects/switch_subs_autoshow_on.png) | ![](/config-effects/switch_subs_autoshow.gif) |

**Auto-download subtitles** — fetches a sidecar subtitle from the subtitle endpoint
when a file opens.

| Off | On | Transition |
| --- | --- | --- |
| ![](/config-effects/switch_subs_autodl_off.png) | ![](/config-effects/switch_subs_autodl_on.png) | ![](/config-effects/switch_subs_autodl.gif) |

## CLI flags

**`--headless`** — the player runs without a window. The on frame is deliberately the
empty desktop crop: no window appeared.

| Off (windowed) | On (no window) | Transition |
| --- | --- | --- |
| ![](/config-effects/cli_headless_off.png) | ![](/config-effects/cli_headless_on.png) | ![](/config-effects/cli_headless.gif) |

**`--no-shortcuts`** — the whole keyboard layer is off. The stills are the same `m`
press with and without the flag: in the normal run the chip reads `Unmute`, with the
flag it stays `Mute`. Both runs are paused at ~1 s so the frames behind the controls
nearly match; the pair is a two-run comparison, and every other GIF on this page is
the "off" state of this flag.

| Off (flag off: `m` mutes) | On (flag on: `m` does nothing) | Transition |
| --- | --- | --- |
| ![](/config-effects/cli_no_shortcuts_off.png) | ![](/config-effects/cli_no_shortcuts_on.png) | ![](/config-effects/cli_no_shortcuts.gif) |

## Flags without a GIF

| Flag | Why no GIF |
| --- | --- |
| `--cast-transcode` | Its effect is the transcode path used *while casting*; nothing changes on the local window until a receiver is attached, and the cast probe is a heavier separate fixture. |
| `--open-dialog` | Summons the system file picker, a native dialog surface this in-app pipeline does not drive. |
| `--no-resume` | Needs a seeded resume position first, and its only visible effect is the starting position clock rather than a window state. |
| `--gui` | Forces the window when launched without a media source; the default launch already opens the window, so there is no distinct on state. |

`--help`, `--version` and `--torrent-list` print to the terminal and exit; they have no
window state to frame.