
# UI design (v0.1 desktop)

> Goal: give the existing “core playback abstraction + FFmpeg backend” its first usable desktop UI — able to open, play, pause, seek, switch tracks and show media information. This document first distils the research conclusions about existing players, then gives the information architecture, control interactions, visual style and technology choice, as the implementation basis; v0.2+ extensions (playlist page, thumbnail preview, mini mode) only have their boundary drawn, not their design.

## 1. Research conclusions

Subjects: IINA (a modern macOS player), mpv.net (a Windows desktop player), vidstack / plyr (web players), the OSC scripts collected by awesome-mpv, and Qt6+QML player implementations as references. Everything below is a **verifiable fact** extracted from repository READMEs/manuals/source defaults, not conjecture.

Re-check entry points (the defaults all live in source, so they go stale on a revision and are worth re-checking): IINA's `iina/Preference.swift` (the `defaultValue` dictionary and the various `defaultValue` static members, such as `controlBarAutoHideTimeout: Float(2.5)`, `thumbnailWidth: 120`, `maxThumbnailPreviewCacheSize: 500`) and its `develop` branch; mpv.net's `src/MpvNet/App.cs` (`RecentCount = 15`) and `src/MpvNet.Windows/WinForms/MainForm.cs`; vidstack/plyr's default layout sources and theme tokens (`--plyr-*` / `--media-*`).

### 1.1 IINA (github.com/iina/iina)

- The OSC (on-screen controller) by default **floats centered** above the bottom edge (`oscPosition = floating`, `controlBarStickToCenter = true`), and **auto-hides after 2.5 seconds without input** (`controlBarAutoHideTimeout = 2.5`, on by default).
- **A single click on the video area shows/hides the OSC** (`singleClickAction` defaults to `hideOSC`), while a double click toggles fullscreen — yielding “single click pauses” to “double click fullscreen”, so the second click of a double-click gesture cannot misfire a pause.
- Wheel semantics are separated: **vertical wheel = volume, horizontal wheel = seek** (the `verticalScrollAction = volume` / `horizontalScrollAction = seek` defaults).
- **Thumbnail preview** on progress-bar hover is on by default (`enableThumbnailPreview = true`, thumbnails 120px wide, cache capped at 500).
- Music mode: on detecting pure audio it **automatically switches** to a mini window (`autoSwitchToMusicMode = true`, showing album art).
- Resume and history are all on by default: `resumeLastPosition`, `recordPlaybackHistory` and `recordRecentFiles` are all true; at the end of playback the window **stays open** (`keepOpenOnFileEnd = true`).
- The playlist is a sidebar inside the main window (third-party plugins doing a “detached playlist window” are precisely the counter-evidence that the built-in form is a sidebar).

### 1.2 mpv.net (github.com/mpvnet-player/mpv.net)

- No permanent toolbar: it uses an **OSC + flat design**; the main menu is a **right-click context menu** (customisable hierarchies via `#menu:` in input.conf).
- **Dark is the default**: `--dark-mode=always` (always by default), and the theme (`--dark-theme`) is independently swappable.
- Recent files: **15** are remembered (`--recent-count` defaults to 15); with an empty playlist the `play-pause` command automatically loads the most recent one.
- OSC dead zone: dragging the window / opening the menu / hiding the mouse only take effect in the window's **central region** (10% around the edges and 22% at the bottom are left to the OSC), avoiding a fight with the controls over events.
- Configuration uses a **searchable GUI settings editor** instead of hand-editing a conf file.

### 1.3 mpv's OSC scripts (collected by awesome-mpv)

- **uosc / ModernX / ModernZ / osc-modern** share one form: a floating bottom bar + a large seek bar + transport on the left and settings on the right, with configurable buttons on the bar.
- Common seek-bar enhancements: a **hover time tooltip** (almost every OSC), **chapter marks** (the chapters script), **hover thumbnails** (tethys, thumbnail_script, and thumbfast as a high-performance thumbnail service).
- The minimalist school uses a **permanently visible thin progress bar** (progressbar, mfpbar — small, out of the way, draggable), showing that “information density can be downgraded” is a valid form.
- uosc also ships a history menu and a pause indicator (pause-indicator), confirming that the “overlay + menu” combination scales.

### 1.4 Web players (vidstack / plyr)

- Control-bar form (from vidstack's default video layout source): **two rows** — the first row is a time slider spanning the full width, the second is a button row (play → volume → time → subtitle toggle → settings menu → fullscreen); the slider gets the full-width grab area and the buttons have their own density. The single-row three-part form (transport left / seek center / settings right) is plyr's.
- Seek-bar interaction details: **hovering shows the target time, dragging previews live, and only releasing commits** (vidstack scrub-preview); keyboard stepping is `keyStep` with a Shift×5 multiplier (the vidstack slider default); vidstack's default seek step is 10s and plyr's `seekTime` is 10s too.
- Settings-menu organisation: vidstack uses a single gear menu containing **five submenus — speed → quality → accessibility → audio track → subtitle** (single-select groups); plyr's settings panel groups “subtitle/quality/speed” with a back-to-parent action. The current value is ticked, and nesting never exceeds one level.
- Keyboard shortcuts converge strongly (the YouTube family): `K/Space` play-pause, `J/L` ∓seekStep (vidstack), `←/→` short seek, `↑/↓` volume, `M` mute, `F` fullscreen, `C` subtitles, `0–9` percentage jumps (plyr semantics; mpv leaves 1–8 to quality adjustment, which is counter-intuitive for ordinary users), `</>` speed up/down (vidstack).
- Both treat accessibility as a first-class citizen: sliders expose `aria-valuemin/max/now/text`; toggle buttons carry `aria-pressed` plus paired pressed/unpressed icons and labels (plyr's "Play"/"Pause"); menu buttons carry `aria-label`; there is a visible focus state (focus-visible) and a dedicated focus-ring token (vidstack's `--media-focus-ring`); and shortcuts are mirrored automatically onto the controls' `aria-keyshortcuts`.
- Theming runs entirely through design tokens: every vidstack token has a `--media-*` semantic name plus a `--video-*` layout override layer, while plyr is a flat set of `--plyr-*` properties (including `--plyr-color-main`, `--plyr-focus-visible-color`, and the full set of control sizes/spacing/radii). Control-button hover tooltips are for screen readers only by default (the only visual hint is the seek time) — keeping the interface quiet.

### 1.5 Qt6+QML desktop implementation reference

- The common pattern proven by projects such as Qt-Music-Player: **a C++ engine layer (playback/queue/metadata) exposed to the QML view layer through QObject properties and signals**, with pages (playback/list/settings) pure QML and animations/theming using QML's native capabilities. The lesson for this project: **the UI layer must depend only on the Player facade** (we already have the equivalent “core/UI” split), so swapping the UI framework does not touch the core.

### 1.6 Cross-framework design decisions (the implementation basis for this document)

1. Control bar: floating at the bottom, content centered, auto-hiding after ~2.5s; **pinned while the pointer is over the bar / the seek bar is being dragged / an overlay is open / playback is paused** (the IINA + vidstack consensus).
2. A single click on the video area toggles the control bar, a double click toggles fullscreen (IINA semantics, avoiding a double-click misfiring a pause).
3. Vertical wheel = volume, horizontal wheel / Shift+wheel = seek (the IINA default).
4. The control bar has two rows: the full-width seek bar above, the button row below (the form that vidstack's default layout and IINA's floating controller both converge on); secondary selections fold into popup menus (the vidstack/plyr/uosc consensus).
5. Seek bar: hover time tooltip + live drag preview + **commit on release**; thumbnail preview is a v0.2 enhancement (IINA's 120px width and 500-image cache are a usable reference).
6. Shortcuts take the union of YouTube/mpv: `Space/K`, `←/→ ±5s`, `Shift+←/→ ±1s`, `PgUp/PgDn ±60s`, `↑/↓ volume`, `M`, `F`, `0–9 percentage`, `A audio track`, `C subtitle`, `I info`, `R recent`, `H help`.
7. The dark theme is the default and is not switchable (the reasoning behind mpv.net's `dark-mode=always` default: a video player's background is naturally black, and a light UI has never been the correct default).
8. Recent items are recorded by default (both IINA and mpv.net have it on by default, at a scale of 15), as the seed data for v0.2's playlist.

## 2. Information architecture (v0.1)

v0.1 is a **single-window application**: one playback page carries everything, and all secondary functions are overlays, with no multi-page routing.

**No top bar**: the main window has no permanent top bar — the brand and the title go through the OS window title, and every entry point lives in OSC row 2 and the shortcuts (§3.2); adding a top bar would introduce permanent chrome area, conflicting with the §1.6-1 research conclusion “floating, content-centered, auto-hiding”. The v2 prototype used to have a top bar and the 2026-10-15 patrol round decided by proxy to remove it; **the top bar and the light theme are both v0.2 candidates, not v0.1 baseline**.

```
soar main window
├── Video area (letterboxed black, scaled proportionally)
│   ├── Pause indicator (large icon, 1s fade)          ← simplified to status text in v0.1
│   ├── Buffering indicator (top-left "Buffering...")     ← driven by BufferingStarted/Ended events
│   ├── Download indicator (top-left "Downloading N%")    ← driven by DownloadProgress, hidden once the cache fills
│   ├── OSD toast (top-centered, 800ms fade)     ← feedback for volume/speed/track changes
│   └── No-media poster area (file name + status text)      ← for pure audio / the null backend
├── OSC control bar (floating at the bottom, auto-hiding, two rows)
│   ├── Row 1: seek bar (full width, hover time tooltip, drag preview, commit on release)
│   ├── Row 2: ⏯ play/pause, ⏹ stop, current/total time | 🔇/🔊 + volume bar,
│   │         speed, audio track, subtitle | ⓘ info, ⛶ fullscreen
│   └── Popup menus: speed / audio track / subtitle (one group each, current item ticked)
├── Info overlay (I key): file, state, duration, seekability, backend, track list (type/index/codec/language/title/selected)
├── Recent overlay (R key): MRU list, click to reopen; a hint when there is no history
├── Media library overlay (B key): watched-folder row + source entries (resume position annotated, offline NFO sidecar title preferred), click to reopen; "Add folder..." goes through watch-mode DirPick
├── Shortcut help overlay (H key): the key column is rendered live (follows the current bindings, byte-identical to the factory table under the default config)
├── Play queue overlay (P key, v0.2): click a queue entry to play, `x` to delete, tri-state Loop and Shuffle
├── Casting overlay (t key): SSDP-discovered renderers, click to cast, phase/position readout, Stop casting/Search again; the CLI goes through `--cast=<location|auto>`
├── Subtitle settings overlay (v0.2): font-size slider, sync offset, visibility toggle
└── Exit paths: Esc (leaves fullscreen first when fullscreen) / the window close button
```

**Entry points for opening media**: a command-line URI (including multiple files enqueued), dragging a file onto the window (the SDL drop event), the recent overlay, and the **empty main window** (the state a GUI start with no source begins in: the `O` key / clicking the poster opens a file, Windows going through a real `GetOpenFileNameW` dialog and other platforms degrading to a “drag and drop to open” toast; BUGS.md #3 — a GUI double click is a launch, not a command line).

**Track and subtitle selection**: the OSC popup menus (current item ticked); the `A`/`C` shortcuts cycle (mpv semantics), with `C`'s cycle including “off”. Subtitle rendering has landed (a libass canvas, faithful styles for embedded ASS and default styles for external text tracks, degrading to plain text in builds without libass); selecting an embedded subtitle track is **decode semantics** — the decode thread swaps the subtitle decoder at a safe point and cues continue streaming from the new track (details and boundaries in docs/mvp.md §6), no longer the earliest “metadata selection”.

**Settings**: v0.1 has no settings page. Theme constants, the auto-hide duration (2500ms), the toast duration (800ms) and the recent cap (15) are defined in one place, and v0.2 turns them into a panel.

**State model**: the UI's visible state = player state (`PlaybackState`) × input state (control-bar visibility, toast, buffering flag) × overlay state (info/recent/help, mutually exclusive, one at a time). Backend errors appear as a toast plus an error row inside the info overlay, never as a modal dialog (player errors rarely warrant interrupting treatment).

## 3. Controls and interactions

### 3.1 The OSC auto-hide state machine

```
visible ──(2500ms without input and not pinned)──▶ hidden
hidden ──(any key / mouse move / wheel / click)──▶ visible (timer reset)
pin conditions (any one of these prevents hiding):
  the pointer is on the control bar or its popup menus / the seek bar is being dragged / any overlay is open / the player is Paused / Stopped / Error
```

Note: “mouse move” now holds in the state machine — SDL_MOUSEMOTION has counted as a wake-up since this round's OSC batch
(player_window.cpp:712), and that line matches the code as it stands.

- IINA takes 2.5s as the default; pinning while paused is adopted from vidstack (when paused the user is about to act, so hiding is counter-intuitive).
- While the seek bar is being dragged nothing is committed, only the preview time is shown; `Player::seek` is called only on `IsItemDeactivated` (release) — the FFmpeg backend's seek is executed asynchronously by the decode thread, so committing continuously during a drag is both wasteful and jittery (the same semantics as vidstack scrub-preview).

### 3.2 Shortcut table (configurable, this table is the default)

| Key | Action | Source |
|---|---|---|
| `Space` / `K` | Play/pause toggle | mpv + YouTube |
| `←` / `→` | Seek ∓5s | mpv default |
| `Shift+←` / `Shift+→` | Seek ∓1s (precise) | mpv default |
| `PgUp` / `PgDn` | Seek ∓60s | mpv's UP/DOWN semantics moved to the Page keys |
| `Home` | Jump to the start | mpv |
| `0`–`9` | Jump to 0%–90% | plyr semantics (mpv's 1–8 quality adjustment is counter-intuitive) |
| `↑` / `↓` | Volume ±5% | YouTube/vidstack/plyr |
| `M` | Mute toggle | the mpv/plyr consensus |
| `,` / `.` | Speed down/up (v0.2) | vidstack `</>` semantics |
| `F` | Fullscreen toggle | the mpv/plyr consensus |
| `L` | A-B loop in three stages: set A → set B → clear (v0.2) | mpv semantics |
| `A` / `C` | Cycle audio track / subtitle track (including off) | making mpv's `#`/`j` memorable |
| `V` | Subtitle visibility (v0.2) | IINA/plyr semantics |
| `[` / `]` | Subtitle sync offset ∓0.5s (v0.2) | mpv semantics |
| `S` | Screenshot the current frame as PNG (v0.2) | mpv semantics (S yields to the screenshot, visibility goes to V) |
| `P` | Play queue overlay (v0.2) | IINA playlist habits |
| `N` / `Shift+N` | Queue next / previous (v0.2, no wrap-around out of range) | mpv semantics |
| `O` | Open a file (Windows file dialog; other platforms degrade to a drag-and-drop hint toast, BUGS.md #3) | turning mpv/IINA's `Ctrl+O` habit into a single key |
| `B` | Media library overlay: watched folders and playback history, click to reopen (including a resume jump) | IINA media-library habits |
| `I` / `R` / `H` | Info / recent / shortcut help | IINA info-panel habits |
| `t` | Casting overlay (DLNA control point): SSDP-discovered renderers, click to cast, phase/position readout, Stop casting/Search again | this project's own (docs/mvp.md §3 casting) |
| `Q` | Quit the program (v0.2) | mpv semantics |
| `Esc` | Leave fullscreen when fullscreen; otherwise quit the program | mpv.net/vidstack semantics, compatible with the existing CLI test contract |

#### Configurable bindings (factory defaults + manual rebinding)

The table above is the factory default (the `ShortcutMap` construction table in
`src/app/ui/shortcuts.{h,cpp}`, whose `ShortcutAction` enum order is the help
overlay's row order). Bindings can be rebound through a hand-edited config file,
which by default lands at `soar/shortcuts.txt` in the state directory (the same
directory as recent.txt/library.txt; Windows uses
`%APPDATA%\soar\shortcuts.txt`); the CLI takes `--shortcuts=FILE` to use another
file and `--no-shortcuts` to turn it off entirely (which outranks a
`shortcuts = on` in the file):

```text
# comment (whole line, or after a value)
play_pause = space, k      # comma-separated; the line replaces that action's default binding
percent_seek = 0-9         # a single-character range expands to 10 keys
screenshot = ctrl+alt+s   # shift+/ctrl+/control+/alt+ prefixes, in any order
quit =                     # an empty value unbinds that action
shortcuts = off            # global switch (on restores the default table)
```

Matching rules (reproducing the original hard-coded switch's behaviour): a binding
without modifiers applies under any modifier state (`shift+q` still quits); a
binding with modifiers takes precedence over a modifierless binding when the
modifiers match (`shift+←` is a 1s seek and `ctrl+o` opens a folder). When the same
key is declared on two lines the later line wins (the later line takes the key away
from the earlier name, so `mute = k` does not lose to play_pause's default `k`).
A line whose tokens are all misspelled is skipped as a typo (it will not
accidentally unbind), and unbinding uses only an empty value. The help overlay's
key column is rendered live from the current bindings (the help text follows a
rebind).

### 3.3 Mouse and wheel

- Single click on the video area: toggle the OSC (IINA's `singleClickAction`); double click: fullscreen toggle (mpv). It is decided by a second click within 500ms (SDL's double-click event semantics). On the empty main window (the no-source starting state) a single click is the open-file entry point (BUGS.md #3) and does not go through the OSC/fullscreen decision.
- Vertical wheel: volume ±5% (the IINA default); horizontal wheel or `Shift+wheel`: seek ±5s.
- Drag a file onto the window: open that file (SDL_DROPFILE).
- Dragging the seek/volume bar: preview the value live (time / percentage), with the seek committing on release.

### 3.4 Hover preview

- v0.1: hovering the seek bar shows a target-time tooltip (ImGui's native tooltip). During a drag it follows above the bar showing the preview time and the visual hint of “commit on release” (colour distinguishes cached from uncommitted: the preview state uses the accent colour).
- v0.2 (boundary): hover thumbnails (referencing IINA's 120px width / 500-image cache and thumbfast's detached thumbnail-service architecture — the decoder produces frames on demand on a background thread and the UI only consumes textures).

### 3.5 Gestures and mini mode (v0.2 boundary)

- Mini mode: the IINA music-mode form (`autoSwitchToMusicMode` defaults to true), shrinking to a small window with cover art for pure audio; done after subtitle rendering/playlist.
- Trackpad gestures (two-finger seek/volume) wait until the keyboard is configurable.

## 4. Visual style

- **Dark is the default and the only theme** (§1.6-7; the light version of the v2 prototype is a v0.2 candidate, not v0.1 baseline, see the no-top-bar contract in §2). The palette (tokenised, concentrated in the `ui_theme` constants section rather than scattered):
  - Background/letterbox `#0A0B0E`; panel `rgba(16,18,24,0.94)` (semi-transparent over the video); stroke `rgba(255,255,255,0.08)`.
  - Primary text `#E8EAF0` / secondary `rgba(232,234,240,0.55)`; accent (progress, selection, hover) `#4C8DFF`; danger/error `#FF5D5D`.
  - Contrast: primary text over the background > 12:1, secondary text > 4.5:1 (WCAG AA).
- **Layout grid**: a 4px base grid; the OSC is ~80px tall, with 8×6 padding and 8 spacing between controls; overlays cap at 480px wide, the OSC bar caps at 920px, and the row height is 24. The video area is always letterboxed proportionally (the black bars are the background colour).
- **Motion**: OSC visibility and toasts use a 160ms opacity transition (linear interpolation inside the frame loop; `disableAnimations` gets no toggle and is always on in v0.1); there are no translation-type animations (not covering the view with UI over the video is the first principle). The pause/buffering indicator fades over 1s.
- **Fonts**: load a system UI font first (a per-platform candidate list: Linux `Noto Sans CJK`/`DejaVu Sans`, macOS `PingFang`, Windows `Microsoft YaHei`), falling back to ImGui's embedded bitmap font (ASCII only) — no font file is bundled, keeping the number of distributed artifacts at zero new ones.

## 5. Technology choice

### 5.1 Candidate comparison

| Dimension | SDL2 custom drawing | **Dear ImGui (chosen)** | Qt6 Widgets/QML | Web shell (CEF/WebView) |
|---|---|---|---|---|
| Added dependency | None | One header-only library (fetched, MIT) | Hundreds of MB of runtime/toolchain | The largest (CEF ~200MB) |
| Cross-platform consistency | High | High (drawn onto SDL_Renderer) | High (native looks differ) | High |
| Fit with the existing core | Already in use (the video texture path) | **Reuses the existing SDL2 window/renderer/event loop, with zero conflict between the video texture and UI overlay** | Needs a parallel event loop / audio-output integration | Needs an IPC / local-service bridge |
| Cost of a modern UI | Very high (every control hand-written; text needs SDL_ttf or a bitmap font) | **Low (sliders/combos/menus/tooltips/theming out of the box, dark by default)** | Medium (QML is declarative, but the binding layer is a lot of code) | Medium (a separate front-end stack) |
| Performance (UI over video) | Hand-controlled | Immediate-mode redraw each frame, with the video path unaffected | The scene graph costs are separate | One extra hop in the compositor |
| Accessibility | All hand-written | Weak (no screen-reader protocol) — mitigated by **full keyboard coverage** | **Strong (native a11y)** | Strong (ARIA) |
| Test drivability | Event injection is straightforward | Event injection is straightforward (SDL layer) + the logic layer is purely unit-testable | Needs a QTest / Accessibility bridge | Needs a CDP / automation bridge |
| Licence | LGPL (already used compliantly) | MIT | LGPL/commercial dual track | Varied (CEF BSD + a pile of Chromium) |

### 5.2 Conclusion and risks

**v0.1 chooses Dear ImGui (with the SDL2 + SDL_Renderer2 backend)**, because:

1. It is the only option that **reuses the existing chain with zero refactoring**: the SDL2 window, YUV texture presentation and event loop are all kept, and ImGui overlays its drawing on the same renderer through the `imgui_impl_sdlrenderer2` backend, with no compositing conflict between video frames and UI.
2. A single lightweight dependency (MIT), pinned and fetched, not entering the distributed artifact, matching the principle of “dependency choices must hold the licence boundary” (docs/licensing.md).
3. The widget library is usable as-is: sliders (seek/volume), combo menus (speed/audio track/subtitle), tooltips (hover time) and theme tokens put the work into interaction semantics rather than drawing pixels.

**Trade-offs recorded explicitly**:

- **Weak accessibility** is ImGui's hard limitation (no platform a11y protocol). Mitigations: full keyboard coverage (the whole §3.2 table is operable blind), visible focus, and adequate contrast; **when the project needs real screen-reader support, migrate to Qt** — core/UI are already split (§1.5), so the migration does not touch `soar_core`.
- **Visible focus has been implemented (2026-10-15 patrol proxy decision b)**: ImGui keyboard navigation is not enabled (`NavEnableKeyboard` stays off) and the focus model is drawn by the app — the §3.2 shortcut contract is untouched to the letter. Focus moves with Tab / Shift+Tab / Enter (none of the three keys is in the §3.2 table, so there is zero conflict), and arrow keys inside popup menus behave the same way (`handleEvent` early-exits once a popup is open, so the arrow keys are not dual-duty with seeking at that moment). Enter on a focused row replays that row's **own click path** rather than adding a separate dispatch. The ring is the prototype's own 2px accent expanded by 4px (`kFocusRingWidth` / `kFocusRingExpand` in `ui_tokens.h`, the same geometry as ImGui's built-in `RenderNavCursor`). The focus order covers only the existing focusable surfaces: playlist rows, media-library rows, and popup menu items. The search box is still focused with `/` (§3.2), and leaving it with Tab clears `ActiveId`.
- **Immediate-mode redraw each frame**: it runs at the video frame rate (≤60fps), and the extra CPU over retained mode is acceptable; a “skip presentation when there is no input and no new frame” dirty-rectangle throttle can be added later (a v0.2 optimisation, not blocking).
- **Qt is a v1.0 alternative rather than the v0.1 choice**: when playlist management, a settings panel and real accessibility arrive, if immediate mode's code organisation starts to feel awkward, migrate wholesale to Qt6 QML (the IINA-style desktop form) — at that point only the view layer is rewritten, and the event model and the Player facade are unchanged.
- **The web shell is rejected**: introducing a browser runtime (size, startup, local-file security policy, an IPC bridge) for local media playback is a net negative, and its advantage (a web widget library) is not usable for this project.

### 5.3 Code structure (where the implementation lands)

```
src/app/main.cpp            # CLI parsing, backend selection, Player assembly (unchanged)
src/app/ui/ui_state.{h,cpp} # pure logic (no SDL/ImGui dependency): auto-hide state machine,
                            #   RecentStore (MRU×15, atomic write), toast, time formatting
                            # → static library soar_app_ui, shared by the app and the unit tests
src/app/ui/player_window.{h,cpp} # SDL2 window + ImGui OSC/overlays + keyboard/mouse mapping
                            #   (compiled only with SOAR_WITH_SDL2 && SOAR_WITH_IMGUI)
tests/test_ui_state.cpp     # doctest unit tests for ui_state (soar_ui_tests)
tests/test_cli.cpp          # new Xvfb + XTEST keyboard-driven cases (reusing the existing injector pattern)
```

CMake: `SOAR_ENABLE_IMGUI` (ON by default) takes ImGui v1.91.9b (including the `imgui_impl_sdl2` + `imgui_impl_sdlrenderer2` backends). The SDL floor is **2.0.18**: the backend itself requires 2.0.18 (`SDL_RenderGeometry`), and the window layer's key-repeat suppression likewise uses `SDL_KeyboardEvent::repeat`, which only exists from 2.0.18, so `find_package(SDL2 2.0.18 CONFIG QUIET)` folds “too old” into the degradation path as well. The fetch method is an **explicit `git clone --depth 1` into `<build>/_deps/imgui-src`** rather than `FetchContent`: the latter fails configure outright when the fetch does not work, which conflicts with this project's “a missing optional capability degrades” convention (the same as SDL2/FFmpeg); an explicit clone turns the failure into one degradation notice and reuses an existing directory directly. The ImGui sources live in the build directory, never enter the repository and never enter the gcovr measurement surface (`--filter src --filter include` excludes them by construction). CI's `no-imgui` job builds with `-DSOAR_ENABLE_IMGUI=OFF` and runs the window cases, pinning that degradation path. At runtime `io.IniFilename` is set to `nullptr`: every window is `NoSavedSettings`, and having ImGui write `imgui.ini` into the user's working directory buys nothing.

## 6. v0.1 acceptance checklist

- [x] Open (CLI/drag-and-drop/recent list; the O key / poster click on the empty main window and the Windows file dialog were added for BUGS.md #3), play, pause, stop, seek (bar + shortcuts + percentage)
- [x] Volume/mute, speed (six steps, 0.5–2.0)
- [x] Audio/subtitle track menu selection and the `A`/`C` cycles (including turning subtitles off)
- [x] Info overlay (full media information) + buffering indicator + error toast
- [x] OSC auto-hide/pin, single-click toggle, double-click fullscreen, wheel volume/seek
- [x] Recent items: recording (MRU×15), reopening, persistence
- [x] Shortcut help overlay; Esc semantics (leaving fullscreen first)
- [x] Xvfb integration cases driving every function from the keyboard + green ui_state pure-logic unit tests
- [x] The coverage gate does not regress (new code is measured under the existing convention, and the gate rises with the water level)

Coverage details are in [docs/coverage-notes.md](/zh/coverage-notes) §3.6: the unreachable items in `player_window.cpp` are defensive branches that require an injected failure to reach, plus the font candidate paths — not untested interactions.

## 7. Later boundaries (against scope creep)

What v0.2 has landed (items from this section's original “not doing” list that were delivered, recorded here so the boundary does not drift): the play queue overlay (P key: click/delete, Loop/Shuffle — the sidebar form was not built, the overlay is what exists), and the subtitle menu becoming effective once subtitle rendering was connected (the `[download]` and `[translate]` rows are wired, see docs/mvp.md §6).

Still not doing: file dialogs on non-Windows platforms (Windows' `GetOpenFileNameW` has landed for the empty main window / the O key entry, and other platforms degrade to a drop hint — a cross-platform dialog UI was not built), seek hover thumbnails (starting from the 120px/500-image cache specification), mini music mode, a settings panel, and multiple themes.
