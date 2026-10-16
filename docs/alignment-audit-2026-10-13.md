[English](/alignment-audit-2026-10-13) | [中文](/zh/alignment-audit-2026-10-13)

# Prototype alignment acceptance (2026-10-13)

Trigger: todo was 28/28 checked but had no "prototype alignment" item and no screenshot evidence. This acceptance round is based on running screenshots and gives a per-screen three-way verdict (aligned / differences / not implemented). Evidence in `evidence/alignment-2026-10-13/` (exclusive Xvfb :116, `SOAR_UI_BITMAP_FONT=1`, real build-cov3 binary, keyboard driven, fixed frames in the paused state; the library screen verified resume-position readout against a seed `library.txt`).

## Prototype inventory

| # | Prototype | Version | Implementation evidence |
|---|------|------|--------------|
| 1 | `prototypes/redesign/desktop-main-{light,dark}.png` | v2.1 | `desktop-main.png` (playing + OSC paused state), `empty-first-run.png` |
| 2 | `prototypes/redesign/playlist-{light,dark}.png` | v2.1 | `playlist.png` |
| 3 | `prototypes/redesign/library-{light,dark}.png` | v2.1 | `library.png` |
| 4 | `prototypes/redesign/settings-{light,dark}.png` | v2.1 | none (not implemented) |
| 5 | `prototypes/translate-panel.png` | v2.0 | none (not implemented) |

## Per-screen verdicts

### 1. desktop-main (main player window) — differences

Aligned: the video area plus the bottom OSC (seek bar row + controls row, two-row layout), the "Paused" chip, subtitle rendering, fullscreen, play/pause/stop, volume slider, playback rate, audio/subtitle track switching (the prototype puts these on the settings page, the implementation puts them in OSC — full coverage, and stronger), A-B loop (L), subtitle toggle (V), screenshot (S), previous/next track (N/Shift+N) — the whole feature surface is present and the keyboard paths are complete (`help.png` is the evidence).

Differences (item by item):
1. **No top bar**: the prototype has a logo + title + four icons for info/recent/help/fullscreen; the implementation has no top bar, the title goes through the OS window title bar (`soar - <filename>`, player_window.cpp:788), and the four icons' functions are covered by the OSC Info/Full buttons plus the R/H shortcuts.
2. **No status pill**: the prototype shows "now playing time P2P 45%" at the top; the implementation has none (equivalent information is in the Info overlay and the OSC time display).
3. **OSC control shape**: the prototype has large round prev/play/next buttons + remaining time `-00:02:14` + a quality pill (1080p 60) + A-B/CC/screenshot buttons; the implementation has Play/Stop + time text + Mute/slider + 1×/Audio/Sub dropdowns + Sub/Info/Full buttons — prev/next/A-B/CC/screenshot originally had only keyboard paths (L/S/V/N/Shift+N, `help.png` is the evidence) and no OSC buttons; no remaining time, no quality pill.

   The OSC batch landed this round: prev/next (`|<`/`>|`), the A-B and Shot screenshot buttons, and remaining time `-mm:ss` are all in OSC now (player_window.cpp:1942-1944, :1946-1956, :1999-2004). (Second OSC batch: the CC button is in OSC, placed between A-B and Shot — i.e. the prototype's "A-B/CC/screenshot" group (player_window.cpp:2050-2052, action = the V subtitle-visibility toggle, toast "Subtitles on/off"; the OSC walkthrough point 875,495 was verified both ways; Shot shifted right by one button width because of the CC insertion, pin 888→917. The quality pill was classified as prototype-ahead v0.2+, see classification item 5 below.)

Alignment addendum (an initial misjudgment, retracted): **the OSC 2.5s auto-hide is implemented** — `HudVisibility` (ui_state.cpp:91, kHideDelay=2500ms, the prototype's value / IINA's default), pinned open by hover/drag/overlay/paused, single-click Force toggles, 0.16s fade out (player_window.cpp:835). Every initial screenshot was taken in the paused state, which pins OSC open, so "always visible" was misjudged. The playing-idle evidence frames `osc-playing.png`/`osc-hidden.png` were held up by the six-tree gate occupying the build tree, and are now added to evidence/ (2026-10-14: Xvfb :118 1280x720 root screenshots, null backend; both frames are in the playing state — the "Playing" label is the proof; the hidden frame was taken after the pointer left the window and the 2.5s idle fade finished, with `event: osc=hidden` in the app log as corroboration; the null backend's position clock does not advance, so both frames read 00:00 — a fixture artifact). Remaining small difference: **mouse movement does not count as input** (SDL_MOUSEMOTION is not handled, only key/wheel/click wake the OSC; the prototype's "no input" includes movement) — registered in todo. (OSC batch this round: SDL_MOUSEMOTION is now counted as an activity (player_window.cpp:712), so mouse movement counts as input and this item is closed.)

### 2. playlist — differences

Aligned: list + current-row highlight (`> ` marker + highlighted row), three-state Loop (off/all/one cycle button, player_window.cpp:2442; the prototype uses a segmented control 关/全部/单曲), Shuffle toggle, remove row (×), open folder, and directory grouping tree (v0.2.1 — an enhancement the prototype does not have).

Differences (item by item):
1. **Shape**: the prototype is a full page with a left sidebar (logo + four feature icons); the implementation is a centered overlay (single-window overlay IA, as ui-design §2 specifies).
2. **Row information**: the prototype has thumbnail + index + duration + a format line (codec · size); the implementation has only file name + ×.
3. **No "add file" button**: the O shortcut opens a file, but the overlay only has "Open folder...".
4. **No per-row hover actions**: the prototype reveals "play next / remove" on hover; the implementation only has the always-visible ×.
5. **No clear-queue button** (the prototype has a trash can top-right).
6. **No bottom mini-player** (the prototype shows the current item plus a progress bar at the bottom).

### 3. library (media library) — differences (the largest structural gap)

Aligned: entries carry resume progress (the implementation's inline `(at 00:45) 01:02` versus the prototype's "downloaded 45%" status line — seed data confirms the readout is correct), folder scanning into the library (watched folders), click an entry to play, and Add folder / Rescan management entries.

Differences (item by item):
1. **No category sidebar**: the prototype has left-side 全部/最近加入/续播/已看完 plus 本地文件/网络源 with counts; the implementation has no categories (the data layer already has position/duration/last_played/play_count fields; categories are view-layer work).
2. **No card grid**: the prototype has thumbnail cards plus chips (P2P/HLS/.torrent/resume); the implementation is a plain text list.
3. **No search box**: the prototype has "搜索媒体…" top-right with `/` to focus; the implementation has none.
4. **Library scope**: the prototype's library includes network/P2P entries (chips distinguish the source type); the implementation's list only holds locally scanned files.

### 4. settings — not implemented

ui-design §2 states plainly "settings: v0.1 has no settings page … v0.2 will make it a panel". None of the prototype's five navigation groups (general / playback / subtitles / network & downloads / about) or its subtitle page (font / size / duration alignment / rendering style / external directory / auto-download / endpoint configuration) exists as UI. Some capabilities live elsewhere: subtitle track selection is the Sub combo in OSC (`drawTrackCombo`, player_window.cpp:2027, call site :1986); the "Subtitle Settings" overlay (invoked by the OSC Sub button, :2791) handles size/offset/visibility; rate and volume are in OSC.

### 5. translate-panel (subtitle translate overlay) — not implemented

No Translate overlay and no shortcut (ShortcutAction has no such entry); the backend has an export→translation pipeline, but no panel UI (endpoint status, progress, and per-line translated preview are all unimplemented).

Addendum: the panel UI is still not implemented; but the subtitle menu and Subtitle Settings overlay's `[translate]` row and the `translateTextAndLoad` pipeline are wired up (player_window.cpp:1174-1183, :1255; the endpoint and model are injected through the `SOAR_TRANSLATE_ENDPOINT`/`_MODEL` environment variables, and the row is not rendered at all when they are unset). The export → translate → write to disk → load chain is fully working (docs/mvp.md §6). The ShortcutAction enum really has no Translate entry (shortcuts.h:116-141), so "no shortcut" still holds.

## Screens the implementation goes beyond the prototype (not drawn in the prototype, not part of the alignment verdict)

Info overlay (`info.png`), Recent overlay, Help overlay (`help.png`), Cast overlay (`cast.png`), empty first-run drop hint (`empty-first-run.png`), and the playlist directory grouping tree — all defined by the ui-design §2 IA or produced by later batches (casting / library).

## Difference classification

### Needs implementation (registered in todo as the "prototype alignment acceptance batch")

1. OSC: mouse movement counts as input (wake the hidden OSC; SDL_MOUSEMOTION→noteActivity).
2. OSC: add prev/next buttons.
3. OSC: add the A-B and screenshot buttons.
4. OSC: remaining time on the right (-mm:ss).
5. Library category sidebar (all / recently added / in progress / watched + counts).
6. Library search box (`/` to focus).
7. Playlist add-file button + clear-queue button.

### Prototype ahead of the spec / suggest changing the prototype (not implemented; the prototype side should carry milestone marks)

1. **settings page**: ui-design §2 sets v0.1 as no settings page and v0.2 as a panel — the prototype is a v0.2 planning draft. Suggest marking `prototypes/redesign/index.html` as "v0.2 planning draft".
2. **translate-panel**: the backend pipeline exists, the UI panel does not — suggest marking it v0.2+.
3. **Top bar / status pill**: equivalent information lives in the OS window title + Info overlay + OSC time, and single-window overlay IA is what ui-design §2 sets — suggest either adding a desktop main screen with overlay IA to the prototype, or listing the top bar as v0.2+.
4. **Thumbnails / card grid / P2P·HLS·.torrent chips**: the data layer has no thumbnail cache and the library view does not distinguish source types — suggest marking them a v0.3 enhancement; todo only takes the category sidebar and search (the data is already there).
5. **OSC quality pill (1080p 60)** (proxy-decided, patrol round R2: classified as prototype-ahead v0.2+) — MVP §1.5 does not list it; the media dimensions (width/height/frame rate) are absent from both `MediaInfo` and `TrackInfo`, so adding it would move the frozen core interface (backend.h); and the second OSC button row is already full once CC is placed (Shot's right edge touches the bar edge), leaving no room. Same family as the top bar / status pill: suggest marking v0.2+ and doing it together with dimension display in the Info overlay (data-layer extension separated from display).

## Conclusion

0 screens aligned / 3 screens with differences (desktop-main, playlist, library) / 2 screens not implemented (settings, translate-panel). A fully checked todo does not mean alignment — before this acceptance round there was genuinely no alignment evidence at all; now 7 screens have screenshot proof, 7 implementation differences are registered in todo, and 5 prototype-ahead items have draft-change suggestions. desktop-main's OSC auto-hide was verified as implemented by reading the source (the initial "always visible" verdict came from paused-state screenshots and has been retracted): paused/hover/drag/overlay all pin the OSC, so acceptance evidence must distinguish pinned states from timed states.

**Patrol round R2 closeout (2026-10-14)**: all 7 "needs implementation" items checked (last two: Playlist add/clear 083cebf, OSC CC a6ff677). desktop-main's OSC difference surface (prev/next, A-B, CC, screenshot, remaining time) all landed, with only the quality pill left, classified as prototype-ahead v0.2+ (classification item 5). The remaining differences in playlist and library (row information, hover actions, mini-player, card grid, chips) all fall under milestones ui-design/mvp already set (v0.2+/v0.3) and are outside this batch. The prototype-side milestone marks for the five "prototype ahead" items landed as suggested (2026-10-14, patrol round): `prototypes/redesign/index.html` gained a "prototype-ahead milestone marks" section marking each item, and the `prototypes/translate-panel.html` page header is marked v0.2+.
