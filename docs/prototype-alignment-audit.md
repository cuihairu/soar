
# Prototype Alignment Audit (2026-10-15)

Trigger: the user reported "soar looks nothing like the design prototypes". This round is **audit only — no
implementation changes, no prototype changes**.
Conclusion up front: **the main cause of "looks nothing like" is not missed implementation work, but the fact
that the repository simultaneously contains three mutually conflicting design baselines — the implementation
strictly follows one of them (`docs/ui-design.md`), while what the user saw is another (`prototypes/redesign/`).**
The genuinely missed items are concentrated in 3 components (the status chip, the resolution pill, the OSC row 1
timecodes); the remaining differences are either intentional deviations with documented rationale, or cases where
the prototype is ahead of the v0.1 scope.

> **Revision (2026-10-15 R1015D patrol-round recheck)**: the initial version's "status chip missed" and
> "resolution pill missed" findings were **misjudgments, now corrected** — ① the status chip is implemented:
> `drawChips()` (`player_window.cpp:2199-2225`, called every frame at `:882`) draws four small top-centered
> chips, Paused / Buffering… / Downloading N% / Casting; all four semantics — status, buffering, download
> percentage, casting — are present, and the only difference from v2 is form (separate small chips vs a single
> chip containing a clock); ② the resolution pill was already decided by proxy not to be done in the MK_R2A
> patrol round (`MediaInfo` has no width/height, the core interface is frozen, MVP §1.5 does not list it, and
> OSC row 2 is already full), and the initial version failed to check that decision. After the revision **the
> only genuinely missed layout item left is the OSC row 1 timecode position** (a small change, but it moves the
> seek bar geometry and requires a walkthrough coordinate recalibration); all remaining differences are
> intentional deviations, deferred by proxy decision, or prototype-ahead cases.

---

## 1. Baseline determination

The repository contains three design baselines, **and they contradict one another**:

| Baseline | Location | Theme | Accent color | Top bar | Status |
|---|---|---|---|---|---|
| **A. v1 prototype** | `docs/public/prototypes/*.html` (5 pages, including `translate-panel.html`) | Single theme | Orange `--accent #ff6b35` + sky blue `--sky` | Yes | **Deprecated**: each v2 page links to it as the "old draft for comparison" (`redesign/desktop-main.html:158`) |
| **B. v2 prototype (final)** | `docs/public/prototypes/redesign/*.html` (4 pages) | Light + dark dual themes + theme toggle | Single signal orange `--accent` | Yes | **Final**: `redesign/index.html:205` "dark four pages pass, finalized"; `docs/prototypes.md:13-16` links only v2 |
| **C. Implementation spec** | `docs/ui-design.md` (254 lines) | **Dark only, not switchable** | **Blue `#4C8DFF`** | **None** | **Implementation basis**: its opening self-describes it as "serves as the implementation basis" |

Determination (per the user's instruction "if `docs/ui-design.md` exists, treat it as authoritative and explain"):

1. **Implementation-side baseline = C (`docs/ui-design.md`)**. It is a written spec, self-described as the
   implementation basis, and the implementation follows it item by item: dark-only (`player_window.cpp:318-352`
   hardcoded, no toggle), blue accent (`player_window.cpp:331`), no top bar (`ui-design.md:61` §2 "v0.1 is a
   single-window app… all secondary functions are overlays"), no general settings page (`ui-design.md:91` "v0.1
   has no settings page; v0.2 will add one as a panel").
2. **Design-side baseline = B (`redesign/`)**. It is the finalized version published on the docs site
   (`docs/prototypes.md` links only to it), and it is the set the user saw.
3. **A (v1) is deprecated**; its only remaining use is the "old draft for comparison" links on the v2 pages.
4. **B and C collide head-on on three axes**: theme (dual themes vs dark-only), accent color (orange vs blue),
   IA (top bar vs no top bar). **The implementation followed C while the user looked at B — hence "looks
   nothing like".**

> This is not "the implementation failed to keep up with the prototype"; it is that **the spec and the prototype
> were never aligned**. `docs/ui-design.md` §1.6-7 explicitly documents the rationale for non-switchable dark
> (citing the mpv.net `dark-mode=always` default), and §1.6-4 documents overlays-first — both are **proactive
> decisions grounded in research**, not cases of overlooking the prototype.

---

## 2. Screen-by-screen comparison (prototype location ↔ implementation location)

### 2.1 Main player window `redesign/desktop-main.html` ↔ `src/app/ui/player_window.cpp`

| Prototype (v2) | Implementation | Match? |
|---|---|---|
| `desktop-main.html:149-163` video surface + subtitle layer (`:30-36`) | `player_window.cpp:188-205` `letterboxRect` letterboxed aspect-preserving fit; subtitle canvas `##subtitle` `:2307` | ✅ |
| `desktop-main.html:165-193` **top bar**: brand `:168-171`, title `:172`, tool group `:173-193` (Info I `:174`, Recent R `:177`, Help H `:180`, Fullscreen F `:183`) | **No top bar**. The title lives in the SDL window title (`player_window.cpp:823`); the four functions sit at the OSC row end `:2054-2071` + shortcuts | ❌ layout |
| `desktop-main.html:189-193` **status chip** `.statechip` (CSS `:63-72`): "Now playing 01:23:30 / 01:25:44 · P2P 45%" | `drawChips()` `:2199-2225` (called at `:882`): four top-centered chips, Paused / Buffering… / Downloading N% / Casting | ⚠️ all semantics present, different form |
| `desktop-main.html:195-202` OSC row 1: left timecode `:197`, seek track (with played+buffer `:198-200`), right timecode `:201`, **resolution pill** `:202` "1080p 60" | Row 1 is a bare full-width slider `:1948-1978`; the timecodes are row 2 text `:2009-2016`; no resolution pill | ❌ layout + missing component |
| `desktop-main.html:204-224` OSC row 2: previous `:205`, play `:208`, next `:211`, volume icon + range `:216-217`, `1.0×` `:219`, A-B `:220`, CC `:221`, Shot `:222` | Play/Pause `:1992`, Stop `:1996`, `|<` `:2000`, `>|` `:2004`, Mute `:2018`, volume slider `:2024-2036`, speed combo `:2038`, audio-track combo `:2043`, subtitle-track combo `:2048`, Sub `:2054`, Info `:2057`, Full `:2060`, A-B `:2065`, CC `:2068`, Shot `:2071` | ⚠️ all functions present, different form |
| `desktop-main.html:255` paused-state 64×64 circular accent play button | 54px text button "Play" `:1992` | ❌ visual |
| `desktop-main.html:154` theme toggle (light/dark) | None | ❌ visual (intentional deviation) |
| `desktop-main.html:234` shortcut annotation (P/I/S) | Shortcuts overlay `:2905` renders the key bindings live | ✅ |
| — (not in the prototype) | Stop `:1996`, Sub `:2054`, Info `:2057`, Full `:2060` — four OSC buttons | ➕ implementation ahead |

### 2.2 Playlist `redesign/playlist.html` ↔ `player_window.cpp:2594` (`Begin("Playlist")`)

| Prototype (v2) | Implementation | Match? |
|---|---|---|
| Standalone page + top bar + list rows (with resume-position markers) | Centered overlay `Begin("Playlist", &open, NoSavedSettings)` `:2594` | ⚠️ form: page vs overlay (`ui-design.md:54` specifies an overlay) |
| Click a queue entry to play, `x` to remove, tri-state Loop, Shuffle | Same semantics (`ui-design.md:74` records them as v0.2) | ✅ semantics match |

### 2.3 Library `redesign/library.html` ↔ `player_window.cpp:2725` (`Begin("Library")`)

| Prototype (v2) | Implementation | Match? |
|---|---|---|
| Watched-folder rows + source entries + resume-position markers | Overlay `:2725`, list-style | ⚠️ same as above (page vs overlay) |
| Card grid, thumbnails, source chips | **None** (`redesign/index.html` marks it v0.3) | ❌ missing component (prototype ahead) |

### 2.4 Settings `redesign/settings.html` ↔ none

| Prototype (v2) | Implementation | Match? |
|---|---|---|
| Settings panel page (one of the four pages) | **No general settings page**. The `Overlay` enum `:183-184` has only `Subtitle`; the sole overlay is "Subtitle Settings" `:2997` | ❌ missing component (intentionally deferred, `ui-design.md:91`) |

### 2.5 Translation panel `prototypes/translate-panel.html` (v1-only) ↔ none

v2 dropped this page (no corresponding file under `redesign/`), and the implementation does not have it either.
**A v1-only item, deprecated together with v1.**

---

## 3. Difference classification (26 items total)

### 3.1 Layout (6 items, including 1 where the implementation is ahead)

| # | Difference | Prototype | Implementation | Nature |
|---|---|---|---|---|
| L1 | **No top bar** (brand/title/Info/Recent/Help/Fullscreen) | `desktop-main.html:165-193` | None; the four functions sit in the OSC `:2054-2071` | Intentional deviation (`ui-design.md:54`) |
| L2 | Status-chip form: v2 has a single chip containing a clock vs the implementation's four separate small chips (Paused/Buffering/Downloading N%/Casting) | `desktop-main.html:189-193` | `drawChips()` `:2199-2225` | Implemented, form difference (initially misjudged as "missed", corrected in revision) |
| L3 | OSC row 1 lacks left/right timecodes | `:197`, `:201` | Row 2 text (old) `:2009-2016` | **Added** (39f7ed5, row 1 = left timecode | track | right -remaining, row 2 clock removed) |
| L4 | **No resolution pill** (1080p 60) | `:202` | None | Decided by proxy not to do it (`MediaInfo` has no width/height, core interface frozen; the initial version missed the MK_R2A decision) |
| L5 | Overlay form: the prototype uses full pages + a top bar, the implementation uses centered floating windows | All four pages | `:2594`, `:2725`, `:2905`, `:2997` | Intentional deviation (`ui-design.md:54`) |
| L6 | Implementation adds four OSC buttons Stop/Sub/Info/Full | None | `:1996`, `:2054`, `:2057`, `:2060` | Implementation ahead (not a deviation) |

### 3.2 Visual (8 items)

| # | Difference | Prototype/spec | Implementation | Nature |
|---|---|---|---|---|
| V1 | **Accent color**: orange `#ff6b35` vs blue `#4C8DFF` | v2 `--accent` | `player_window.cpp:331` | Intentional deviation (`ui-design.md:185`) |
| V2 | **Theme**: light+dark dual themes with a toggle vs hardcoded non-switchable dark | `desktop-main.html:154`, CSS `:45-47`, `:97-101` | `:318-352` | Intentional deviation (`ui-design.md:57` §1.6-7) |
| V3 | **Corner rounding**: `--r-panel 10px` / `--r-ctl 7px` / pill chips / circular icon buttons | v2 tokens | `ui_tokens.h`: `kPanelRounding 10` (window+popup) / `kControlRounding 7` (frame+grab) / `kPillRounding 999` (statechip + paused floatpill, via draw list — `CalcWindowMinSize` raises the window minimum height to `rounding-1`, and routing 999 through `WindowRounding` stretches an auto-sized window to fill the viewport; measured: a 249px pill rendered 540px tall) | Added in d7e7bf6 (the circular icon-button form is not pursued after the V6 reclassification — the spec's button vocabulary is text/emoji, see the V6 row; there are no icon-button consumers today) |
| V4 | **11px font-size floor** was undocumented | v2 convention (`prototypes.md:55`) | `ui_tokens.h` `kMinFontSize 11` + `kUiFontSize 16`, with a `static_assert` floor constraint; ImGui's built-in 13px default also sits above the floor | Added in d7e7bf6 |
| V5 | **Play-button form**: 64×64 circular accent button vs 54px text button | `desktop-main.html:255` | `:1960-2042` | Added in 3835768 (dimming + circular button + hint pill, click to resume) |
| V6 | **Button form**: prototype SVG circular buttons / segmented chips (`.tsmall`/`.seg`) vs rectangular text buttons | `desktop-main.html:205-222` | `player_window.cpp:1992-2071`; button-face rounding already follows v2 via V3 (`kControlRounding 7`) | Intentional deviation (reclassified 2026-10-15 R1015E: the spec's button vocabulary is text/emoji — `ui-design.md:74` §2 IA "⏯ play/pause, ⏹ stop… ⓘ info, ⛶ fullscreen", the sole icon item is explicitly simplified to text `ui-design.md:67`; SVG circular buttons never entered the spec, and actually doing them would first require a spec plus an icon-font decision. The seg chip form was already decided by proxy via I2 (dropdowns) / I5 (OSC buttons)) |
| V7 | **Focus visibility + reduced-motion guard**: the prototype has a `:focus-visible` 2px accent ring and a `prefers-reduced-motion` guard | v2 convention (`prototypes.md:55`) | Split into two items (2026-10-15 R1015E): **V7a animation toggle** — the spec explicitly does not provide one (`ui-design.md:188` §4 "`disableAnimations` gets no toggle, always on in v0.1"), and the current state is exactly what the spec requires; **V7b focus ring** — **decided by proxy 2026-10-15 as (b) and landed** (app-drawn focus model, see §5-P2-8) | V7a intentional deviation (explicitly deferred by the spec); V7b **landed** (`UiFocus` in `ui_state.h`: Tab/Shift+Tab/Enter move focus, and Enter replays the row's own click path; the ring uses `kFocusRingWidth 2` / `kFocusRingExpand 4` from `ui_tokens.h`, drawn on each overlay/menu row; wired via `drawFocusRing`/`focusOverlayKey`/`focusMenuKey` in `player_window.cpp`. The §3.2 contract is untouched by a single entry, `test_shortcuts` 16/16 green; `test_ui_state` 44/44 green; X11 walkthroughs confirmed both the playlist-row ring and the popup-menu ring land in place) |
| V8 | **Design tokenization**: the spec requires "concentrated in the `ui_theme` constants section" | `ui-design.md:183` | There is a concentrated section (`:317` comment "every color the UI draws comes from here") but it is all literal `ImVec4` values, with no named tokens | **Missed work** (half-met) |

> Matching items (not counted as differences): danger/error color `#FF5D5D` (`ui-design.md:185` ↔ `:2417`),
> primary text color `#E8EAF0`, semi-transparent panels over the video, OSC height ~80px / padding 8×6 /
> control spacing 8, overlay width cap 480px, OSC bar width cap 920px, 4px base grid — **the implementation
> matches the spec on every one of these item by item**.

### 3.3 Interaction (4 differences + 2 already aligned + 1 equivalent)

| # | Difference | Prototype | Implementation | Nature |
|---|---|---|---|---|
| I1 | **Info/Recent/Help/Fullscreen entry location**: persistent top-bar icons (title carries the shortcut) vs tucked into the OSC row end + shortcuts | `:174-183` | `:2054-2071` + I/R/H/F | Intentional deviation (usability-equivalent, weaker discoverability) |
| I2 | **Speed/audio-track/subtitle selection form**: prototype OSC segmented chips + full selection on the settings page vs everything as `BeginCombo` dropdowns | `:219-221` + `settings.html` | `:2038-2051` | Intentional deviation (`ui-design.md:54` §1.6-4 "secondary selections fold into popup menus") |
| I3 | **Volume**: icon + range vs Mute button + slider | `:216-217` | `:2018`, `:2024-2036` | Equivalent (not a difference) |
| I4 | **OSC dead zone and auto-hide**: center dead zone, 2.5s auto-hide | `ui-design.md:26` §1.2 | 2500ms auto-hide landed | ✅ aligned |
| I5 | **A-B / subtitles on-off / screenshot**: prototype OSC segmented chips | `:220-222` | Added as OSC buttons `:2065-2071` | ✅ aligned (landed in an earlier batch) |
| I6 | **Previous/Next**: prototype OSC has buttons | `:205`, `:211` | Added `|<` / `>|` `:2000-2004` | ✅ aligned (landed in an earlier batch) |

### 3.4 Missing components (8 items)

| # | Missing component | Prototype | Implementation | Nature |
|---|---|---|---|---|
| M1 | Top bar | `desktop-main.html:165-193` | None | Intentional deviation |
| M2 | Status chip (including the P2P download percentage) | `:189-193` | `drawChips()` `:2199-2225` (Paused/Buffering/Downloading/Casting) | Implemented, form difference (corrected in revision) |
| M3 | Resolution/codec pill | `:202` | None | Decided by proxy not to do it (core interface frozen, MK_R2A) |
| M4 | General settings page | `settings.html` | None (only Subtitle Settings `:2997`) | Intentionally deferred (v0.2) |
| M5 | Library card grid / thumbnails / source chips | `library.html` | List-style `:2725` | Prototype ahead (v0.3) |
| M6 | Mini mode / music mode (auto-switch to a mini window for pure audio) | `ui-design.md:17` §1.1 research | None | Intentionally deferred (§7 future boundary) |
| M7 | Seek hover thumbnail preview | `ui-design.md:55` §1.6-5 | None | Intentionally deferred (v0.2) |
| M8 | Translation panel | v1 `translate-panel.html` | None | Deprecated with v1 |

**Missed-work total (after revision): L3, V3, V4, V5, V6, V7, V8 = 7 items, resolved as 5 landed +
2 reclassified as intentional deviations + 0 still open.** (Landed: L3 39f7ed5, V8 53ae80b, V5 3835768,
V3/V4 d7e7bf6, V7b focus ring 2026-10-15 proxy decision b; reclassified: V6 intentional deviation =
prototype ahead of the spec; V7 split into V7a/V7b with V7a an intentional deviation = explicitly deferred by
the spec. All missed items have landed or been reclassified; there are no open items)

**Intentional deviation/deferral total: L1, L4, L5, V1, V2, V6, V7a, I1, I2, M1, M3, M4, M5, M6, M7, M8 =
16 items.** (V6/V7a reclassified in R1015E)

**Implemented with form differences: L2, M2 = 2 items (misjudged as missed in the initial version, corrected in
revision). Implementation ahead: L6 = 1 item. Verified-aligned items: I3, I4, I5, I6 = 4 items (not counted
as differences).**

---

## 4. Intentional deviations vs missed work

### 4.1 Intentional deviations (documented rationale, **the implementation should NOT be changed to match the prototype**)

| Item | Basis | Notes |
|---|---|---|
| No top bar; the four icons live in the OSC | `ui-design.md:61` §2, `:54` §1.6-4 | "v0.1 is a single-window app… all secondary functions are overlays"; §1.6-4 cites the vidstack/plyr/uosc consensus "secondary selections fold into popup menus". A top bar is window chrome and conflicts with the research conclusion "floating, content-centered, auto-hiding" (§1.6-1) |
| Dark-only, not switchable | `ui-design.md:57` §1.6-7 | Cites mpv.net's `dark-mode=always` default: "a video player's background is naturally black; a light UI has never been the correct default" |
| Blue accent `#4C8DFF` | `ui-design.md:185` §4 | Explicit in the spec; contrast >12:1 already verified |
| Speed/audio-track/subtitle selection via dropdowns | `ui-design.md:54` §1.6-4 | The same clause as "overlays first" |
| No general settings page | `ui-design.md:91` §2 | "v0.1 has no settings page… v0.2 will add one as a panel" |
| No library card grid/thumbnails | `redesign/index.html` marks v0.3 | The prototype itself carries the version tag |
| No mini mode, no seek thumbnails | `ui-design.md:250` §7 | Future boundary |
| No resolution pill | Already decided by proxy in the MK_R2A patrol round | `MediaInfo` has no width/height (core interface frozen), MVP §1.5 does not list it, OSC row 2 is already full — prototype-ahead items were handled per "do not chase prototype-ahead items" |
| Text buttons instead of SVG circular buttons (V6) | `ui-design.md:74` §2 IA, `:67` | The spec's button vocabulary is text/emoji (⏯/⏹/🔇/ⓘ/⛶); the sole icon item is explicitly "simplified to status text"; SVG circular buttons/seg chips are prototype language that never entered the spec (the seg form was already decided by proxy via I2 dropdowns and I5 OSC buttons); v2 rounding has been applied to all buttons via V3 |
| No reduced-motion toggle (V7a) | `ui-design.md:188` §4 | The spec explicitly says "`disableAnimations` gets no toggle, always on in v0.1"; the only motion is a 160ms opacity transition, with no translation-type animations, so always-on does not affect look and feel |

### 4.2 Missed work (v2-finalized design language absent from the implementation, **should be added**)

| Item | Impact | Why it is missed work |
|---|---|---|
| **OSC row 1 timecodes** (L3) — **added** (39f7ed5) | Medium | Row 1 = left timecode | seek track | right -remaining; the row 2 clock was removed to avoid duplication. The cost materialized as predicted: 19 walkthrough pins across row 2/popup layers were recalibrated by a measured -106px (comparing builds before/after the probe), and the drifted audiodrive pins at old 637/685 were re-pinned to their measured positions; the `test_cli.cpp` drag points (cx±80, h-76) were verified to still land on the track and were left unchanged |
| **Tokenization** (V8) — **landed** (53ae80b, named tokens in `ui_tokens.h`) | Medium | `ui-design.md:183` explicitly requires "concentrated in the `ui_theme` constants section"; color values are currently concentrated in `applyTheme` but all as unnamed `ImVec4` literals, and later theme/focus-state/rounding alignment all depend on it as a prerequisite |
| **Focus visibility ring** (V7b, the focus half of the original V7) — **added** (2026-10-15 proxy decision b) | Medium | Required by the spec (`ui-design.md:216` lists "focus visibility" as a mitigation for ImGui's accessibility shortfalls). The proxy decision chose an app-drawn focus model over enabling ImGui navigation: Tab/Shift+Tab/Enter are all absent from the §3.2 table, and arrow keys inside popups take the early-exit path, so there are zero shortcut-contract conflicts; the ring geometry matches the prototype's `:focus-visible` (2px accent expanded by 4px). The reduced-motion half (V7a) was reclassified as an intentional deviation, see 4.1 |
| **Rounding tokens** (V3) — **added** (d7e7bf6) | Low | v2 documents `--r-panel 10`/`--r-ctl 7`; the implementation's 8/6/4/6 is a different set of numbers |
| **11px font-size floor** (V4) — **added** (d7e7bf6) | Low | The v2 convention; the implementation had no documented floor |
| **64px circular play button** (V5) — **added** (3835768) | Low | The first-impression item for "does it look like the prototype", small change |

---

## 5. Alignment plan (by priority)

### P0 (reordered after revision)

1. **Tokenization refactor** (V8): collapse the literals in `applyTheme()` (`:318-352`) into named constants
   (`kAccent`, `kText`, `kPanelBg`…), aligning with `ui-design.md:183`. **Pure refactor, zero geometric/visual
   change, no walkthrough-coordinate impact**, and a prerequisite for V3/V7.
2. ~~Add left/right timecodes to OSC row 1~~ (L3): **added** (39f7ed5). Walkthrough pins recalibrated by
   measurement, 19 places (-106px); full-fixture cli 68/68 · 573 assertions green.
3. ~~Status chip~~: **already exists** (`drawChips()`), withdrawn. ~~Resolution pill~~: **already decided by
   proxy not to do**, withdrawn. Both initial P0 items were misjudgments; see the revision note at the top.

### P1 — Visual details (second wave)

4. **Change the play button to a 64px circular accent button** (V5) — the first-impression item for "does it
   look like the prototype", small change, direct payoff.
5. ~~Focus visibility + reduced-motion semantic toggle~~ (V7) — closed out via the split: V7a reduced-motion is an
   intentional deviation (`ui-design.md:188` explicitly says "no toggle, always on in v0.1"), implementation
   untouched; V7b focus ring landed after the 2026-10-15 proxy decision (b) (see P2-8) and was moved out of P2.
6. **Align rounding/font-size tokens with v2** (V3, V4) — changed together with P0-1 (tokenization).

### P2 — Structural items (**need a user decision, do not touch yet**)

7. **Top bar** (L1/M1): the only item that needs a decision. Three options:
   - **(a) Keep no top bar per `ui-design.md`, and change the v2 prototype to drop the top bar with the
     rationale written out** (recommended). Rationale: the implementation's lack of a top bar is the product of
     the §1.6-4 "overlay" consensus, and the OSC + shortcuts already cover every entry point; adding a top bar
     would introduce persistent chrome area, conflicting with the §1.6-1 research conclusion "floating,
     content-centered, auto-hiding".
   - (b) Add a top bar and update the `ui-design.md` §1.6-7 and §2 IA trees in sync.
   - (c) Hybrid: move only "Fullscreen/Recent/Help" into a top bar, leaving Info in the OSC.
   **Decided by proxy 2026-10-15: (a).** The top bar was removed from all four main-player-window frames of the
   v2 prototype with the rationale written out (the title goes through the OS window title
   `player_window.cpp:855`, and I/R/H/F are all in the §3.2 contract); the top bar/light theme were written up as
   v0.2 candidates, not the v0.1 baseline (`ui-design.md` §2/§4, `prototypes.md` synced); the two
   main-player-window PNGs are the finalized pre-removal frames and were not re-shot (this repo has no rendering
   environment), with the difference annotated.

8. ~~**Focus visibility ring** (V7b): the spec lists "focus visibility" as a mitigation for ImGui's accessibility
   shortfalls (`ui-design.md:216`), but there is currently **no keyboard focus model** (no `ImGuiConfigFlags`
   setting anywhere in the repo, `NavEnableKeyboard` off by default), so the ring has nowhere to be drawn. Three
   options:~~
   - ~~(a) Enable ImGui keyboard navigation…~~
   - ~~(b) An app-drawn focus model…~~
   - ~~(c) Defer to v0.2…~~
   **Until the user decides, this item leaves the implementation untouched.**

   **Decided by proxy 2026-10-15: (b), and landed.** Comparison and verdict for the three options:

   | Option | Verdict |
   |---|---|
   | (a) Enable ImGui keyboard navigation | **Rejected**. `WantCaptureKeyboard` turning true makes the shortcut mapping early-exit (`player_window.cpp:575-660`), and the entire §3.2 table contract would need reworking; `ConfigNavCaptureKeyboard=false` preserves the shortcuts but leaves the arrow keys dual-duty (both seeking and moving focus). |
   | **(b) App-drawn focus model** | **Selected**. Navigation stays off and the accent ring is drawn from our own state — zero shortcut conflicts. Focus movement uses only Tab / Shift+Tab / Enter, and all three keys are **absent from the §3.2 table** (`ui-design.md:114-141`); arrow keys inside popup menus work the same way because `handleEvent` early-exits when a popup is open, so the arrow keys are not dual-duty with seeking at that point. |
   | (c) Defer to v0.2 | **Rejected**. `ui-design.md:216` has the current spec already requiring focus visibility; changing that wording would be a downgrade. |

   Landed as: `UiFocus` in `ui_state.h` (pure logic, no SDL/ImGui dependencies, drivable from unit tests) holds
   surface + row index + an "armed awaiting trigger" tri-state; `ui_tokens.h` pins `kFocusRingWidth 2` /
   `kFocusRingExpand 4` (same geometry as the prototype's `:focus-visible` and ImGui's `RenderNavCursor`);
   `drawFocusRing` / `focusOverlayKey` / `focusMenuKey` in `player_window.cpp` wire into the row drawing of each
   overlay and popup menu. When Enter triggers the focused row, it replays that row's **own click path**
   (`takesActivation` returning true enters the same branch as the `Selectable` result), with no separate event
   dispatch. The focus order covers only the existing focusable surfaces: playlist rows, library rows, and popup
   menu items — overlays are mutually exclusive (§2) and popups belong to the OSC or to one overlay, so only one
   surface holds focus at a time. Search-box focus still goes through `/` (§3.2), and Tab away releases it via
   `ImGui::ClearActiveID()`.

   Verification: `test_ui_state.cpp` gained 7 cases (44/44 · 296 assertions) pinning end-around wrap, empty
   surfaces, shrink clamping, one-shot Enter replay, and arm invalidation; `test_shortcuts.cpp` 16/16 · 213
   assertions green (§3.2 contract untouched); X11 walkthroughs (Xvfb + XTEST + pixel scanning) confirmed the
   playlist row ring advances with Tab, the popup-menu ring advances with the arrow keys, and Enter triggers the
   speed row (OSC `1x`→`0.75x`, popup closes). The mechanism's wording has been written into `ui-design.md` §5.2.

### Explicitly not doing (to avoid making the implementation worse)

- **Switching the accent to orange, adding a theme toggle** (V1, V2): conflicts with the prototype but has
  documented rationale (`ui-design.md:57`, `:185`), and dark + black video background is the mpv.net/IINA
  research consensus. Changing it would make the implementation worse.
- **General settings page** (M4): v0.2 scope, prototype ahead.
- **Library card grid/thumbnails** (M5): v0.3 scope.

---

## 6. Desktop / mobile split

**This repo currently has exactly one UI (desktop).** Neither mobile shell has reached the stage of needing to pick
a prototype:

| Platform | Directory | Current state | Which prototype version it follows |
|---|---|---|---|
| **Desktop** | `src/app/ui/` (C++/SDL2/ImGui) | The only form factor with an implementation | Spec `docs/ui-design.md` (C); the visual language partially takes v2's OSC semantics (prev/next/A-B/CC/Shot already filled in per v2) |
| **iOS** | `ios/` | **Skeleton batch**: the `SoarKit` Swift Package is API-layer only (playback-control facade + state/event model + `NullEngine` state machine); the App shell (SwiftUI) **not started** (`ios/README.md` "current status" table); core cross-compilation not started | No UI to compare against. When the time comes it should take **v2's three-layer tokens** (raw/semantic/component). Note: `ui-design.md`'s "dark not switchable" is a conclusion **in the desktop-window context** (§1.6-7 cites mpv.net) and **does not apply to the iOS native shell** — iOS has the system `prefers-color-scheme`, and the native shell should follow the system |
| **HarmonyOS** | `harmonyos/` | **Skeleton batch**: `Index.ets` is a UI skeleton (brand slot + status text + an "Open file" placeholder); the playback-core route (system `AVPlayer` vs `soar_core` NDK) **awaits a decision**; the project **has not been compile-verified** (no DevEco toolchain) | No UI to compare against. The core route is undecided; alignment discussion should wait until the route is settled |

**Two points to record now:**

1. `docs/ui-design.md` is a **desktop-only** spec (§1.5 explicitly says "the UI layer must depend only on the
   Player facade", framework-agnostic); mobile should not copy its "dark not switchable" verbatim.
2. If a mobile shell goes with native UI, v2's CSS variables **cannot be carried over directly** (no CSS
   variables); each shell needs to land its own token table.

---

## 7. Outdated prototype items (recommend changing the prototype, not bending the implementation)

| Item | Current state | Recommendation |
|---|---|---|
| **The entire v1 prototype** | `docs/public/prototypes/*.html` (5 pages including `translate-panel.html`) has been superseded by v2; `redesign/index.html:205` finalized; `docs/prototypes.md:18` links only v2 | Keep but **explicitly mark as deprecated** (or move to `prototypes/archive/v1/`). The "old draft for comparison" links on the v2 pages (`redesign/desktop-main.html:158`) are its only remaining use. **Handled (8143168)**: all six pages (including the `index.html` overview) prepend "v1 old draft (deprecated, see v2 at …)" to the docbar and link to the corresponding v2 page; **kept, not deleted** (v2's "old draft for comparison" links still point at them, and moving the directory would break them) |
| **Docs-site screenshot references out of date** | `docs/.vitepress/theme/Screenshots.vue` still loads the **v1** PNGs (`prototypes/{desktop-main,translate-panel,settings}.png`), **contradicting** `docs/prototypes.md` linking v2 | **The most substantive documentation inconsistency of this round**: repoint to the v2 screenshots (`redesign/*-{light,dark}.png`) and drop `translate-panel` (v2 no longer has that page). **Handled (8143168)**: the prototype cards now point to the four `redesign/*-dark.png` pages (and add playlist/library cards that had never appeared on the site before), the translate-panel card was deleted; the delivery contract `prototypes/README.md` synced with `prototypes.md` |
| **Translation panel** | v1-only, deleted in v2, absent from the implementation | If translation is still in scope, the page needs to be redone in v2; if abandoned, `docs/prototypes.md` and the todo should stop mentioning it. **Resolved on record (8143168)**: the feature is in scope and has landed (the subtitle menu `[translate]` row, see §6), and the v1 page is kept as a design draft (its header now marks it as a v0.2+ planning draft + not redone in v2); **redoing the page in v2 is design work and is unscheduled**, and the docs site and todo no longer treat it as pending |
| **v2's "light version" positioning** | The v2.1 recheck (`redesign/index.html:206`) changed the light version from "always dark" to "a shell that truly goes light with the theme", but `ui-design.md` still says dark-only | If option (a) of §5-P2-7 is adopted, the v2 prototype and `ui-design.md` must both state "top bar/light theme are v0.2 candidates, not the v0.1 baseline" in sync. **Handled (2026-10-15, synced with the top-bar proxy decision a)**: `ui-design.md` §2 gained a "no top bar" paragraph and §4 a light-version note marking it a v0.2 candidate; `redesign/index.html` gained a proxy-decision section |

---

## 8. Revisions and boundaries

- 2026-10-15 initial version (order: write this file only; no implementation, prototype, or docs-site changes).
- 2026-10-15 R1015D patrol-round recheck revision: withdrew the two misjudgments "status chip missed" and
  "resolution pill missed" (the former is implemented in `drawChips()`, the latter already decided by proxy in
  MK_R2A); missed items went 11→7, P0 reordered.
- Execution order: P0-1 (tokenization) → P1 (visual details) → P0-2 (OSC row 1 timecodes, requires walkthrough
  coordinate recalibration); P2-7 top bar awaits the user's (a)/(b)/(c) decision.
- 2026-10-15 MARKR1 closeout of §7 outdated prototype items: the docs-site "interface preview" prototype cards
  were repointed from v1 PNGs to the v2 finalized screenshots (adding playlist/library cards and deleting the
  translate-panel card), the six v1 pages had deprecation marked in their headers, and the delivery contract
  `prototypes/README.md` was synced with `prototypes.md` (8143168). §7's "v2 light version positioning" still
  follows the §5-P2-7 top-bar decision (only option (a) requires syncing the wording) and was left untouched.
- 2026-10-15 R1015E closeout reclassifications: V6 (button form) reclassified as **intentional deviation** —
  the spec's button vocabulary is text/emoji (`ui-design.md:74` §2 IA), the sole icon item is explicitly
  simplified to text (`:67`), SVG circular buttons never entered the spec, and the seg chip form was already
  decided by proxy via I2/I5; V7 split — V7a reduced-motion is an **intentional deviation** (`ui-design.md:188`
  explicitly defers it), V7b focus ring was kept open and moved into P2-8 (listed with the top bar as a user
  decision; landed after the 2026-10-15 proxy decision (b), see the next entry).
- 2026-10-15 patrol-round proxy decision: P2-7 top bar chose **(a)**; the top bar was removed from all four
  main-player-window frames of the v2 prototype with the rationale written out, and the top bar/light theme were
  recorded as v0.2 candidates, not the v0.1 baseline (§7 row 4 closed accordingly); the two main-player-window
  PNGs were not re-shot (no rendering environment), with the difference annotated in the delivery contract and
  `prototypes.md`.
- 2026-10-15 patrol-round proxy decision: P2-8 focus ring chose **(b)** (app-drawn focus model; (a) rejected
  because the `WantCaptureKeyboard` early-exit would rework the entire §3.2 table contract, (c) rejected because
  `ui-design.md:216` has the current spec already requiring focus visibility), and it landed — `UiFocus` in
  `ui_state.h` + `kFocusRingWidth`/`kFocusRingExpand` in `ui_tokens.h` + `drawFocusRing`/`focusOverlayKey`/
  `focusMenuKey` wiring in `player_window.cpp`; the §3.2 contract untouched by a single entry
  (`test_shortcuts` 16/16 green), `test_ui_state` 44/44 green, and X11 walkthroughs confirmed the playlist-row
  ring and popup-menu ring land in place. The mechanism wording was written into `ui-design.md` §5.2. V7b is
  hereby out of the open items; all 7 missed items have landed or been reclassified.
