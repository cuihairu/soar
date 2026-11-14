
# Audit — documentation consistency (2026-10-13)

> Note (2026-10-16 bilingual pass): this document's line anchors point at the snapshot taken before the
> rename — the Chinese draft is now `*.zh.md` (a language switcher was added at the top, shifting every
> line by +2), and the English drafts sit at the bare names (`docs/mvp.md`, `docs/ui-design.md`,
> `docs/coverage-notes.md`, the last of which only gained a "no translation yet" page this round).
> Read the anchors against the pre-switcher layout.

Audit scope: README.md, README.zh.md, docs/index.md, docs/build.md, docs/mvp.md,
docs/ui-design.md, docs/licensing.md, docs/coverage-notes.md, docs/prototypes.md,
docs/alignment-audit-2026-10-13.md, plus the public-facing surfaces (GitHub description / homepage /
topics and the README opening paragraph).

Method: three difference classes — **ahead** (the doc claims something the code does not have — the most dangerous), **missing** (the code has it, the doc does not — only substantive items recorded), **mismatch** (both sides have it but wording, parameters, defaults, paths or behavior disagree). Every claim carries `file:line` source evidence; negative conclusions are only recorded after confirming the search path was meaningful. Fixes always go **by the code** (delete the ahead claim or mark it unimplemented, add what is missing, correct mismatched wording) — code is never bent to suit a document.

Baseline: `f1b245d` (the transcode fix commit of this round). The OSC batch (mouse movement counting as input, prev/next, A-B, Shot, remaining time) was uncommitted in the worktree at audit time; every row touching it is marked "OSC batch landed this round" and closes once it lands.

> **Closeout note (2026-10-15 re-check)**: all 50 rows A01–A50 are closed — verified line by line against current
> `main` (`572e3ae`): the fixes for A01–A49 all landed (including the "OSC batch landed this round" notes on A31/A32/A37 and the line-number / number re-anchoring on A23–A30), and A50 checked out consistent with no fix needed. This table is now a historical snapshot, no longer a live todo ledger; later documentation drift gets its own audit under the same method rather than additions to this table.

## Counts

| Class | Rows |
|---|---|
| Ahead (docs ahead of code) | 3 |
| Missing (code ahead of docs) | 12 |
| Mismatch | 35 |
| Total | 50 |

## P0 list

1. **README.zh.md:33** "v1.0 (hardware decoding/HDR, casting, media library) has not started" — the media library
   (`src/app/ui/library.cpp:190,396`), the first hardware-decoding wiring (`src/app/main.cpp:200`) and DLNA casting (all of `src/cast/`) all landed, which directly contradicts the English README; three delivered feature areas were misrepresented as not started.
2. **docs/coverage-notes.md:24-25** §2 table "gates 95.0% / 84.6%" — the live gates
   (`.github/workflows/ci.yml:485`, the local six-tree script `soar_sixtree.sh:102`) were
   re-anchored to **94.0 / 83.0** (with the re-set recorded at §3.24:374); the table did not follow.
3. **docs/coverage-notes.md:385** §5 conclusion "line 95.54% and branch 85.15% are the current
   real ceiling of the codebase" — the post-transcode-batch water level is 94.44% (8886/9410) / 83.50%
   (9500/11376) (§3.24:374-375), so the conclusion numbers were stale.

## Difference table

| ID | Page | Claim | Source evidence (file:line) | Class | Severity | Fix |
|---|---|---|---|---|---|---|
| A01 | README.zh.md:33 | v1.0 (hardware decoding/HDR, casting, media library) has not started | Media library `src/app/ui/library.cpp:190,396`; `--hwdec=` `src/app/main.cpp:200`; DLNA `src/cast/{ssdp,soap,transcode_server}.cpp` | Mismatch | P0 | Reword to "two v1.0 slices landed (media library, first hardware-decoding wiring), DLNA casting landed; HDR and plugins not started" |
| A02 | coverage-notes.md:24-25 | Gates 95.0% / 84.6% | `.github/workflows/ci.yml:485`, `soar_sixtree.sh:102` both read `--fail-under-line 94.0 --fail-under-branch 83.0`; re-set record at `docs/coverage-notes.md:374` | Mismatch | P0 | §2 table gate column → 94.0/83.0; measured column refreshed to the §3.24 level 94.44/83.50 |
| A03 | coverage-notes.md:385 | Line 95.54% and branch 85.15% are the current real ceiling | `docs/coverage-notes.md:374-375`: post-transcode level 94.44% (8886/9410) / 83.50% (9500/11376) | Mismatch | P0 | §5 conclusion numbers refreshed to the §3.24 level; 95.54/85.15 marked as the pre-transcode reading |
| A04 | README.zh.md | The whole "media library" feature is missing (watch folders / history / resume / B overlay / NFO) | `src/app/ui/library.cpp:190,396`; `src/app/ui/shortcuts.cpp:207` (kKeyB) | Missing | P1 | Add a "media library" feature entry to features (translate the same item from the English README) |
| A05 | README.zh.md | The video rendering item lacks the `--hwdec=` hardware decoding and software-fallback sentence | `src/app/main.cpp:200-206`; `src/core/ffmpeg_backend.cpp:77-101,2855` | Missing | P1 | Append "--hwdec= requests hardware decoding, falls back to software on failure, hwaccel frames are transferred back as YUV420P" to that item |
| A06 | README.zh.md | Casting (DLNA control point, `t` overlay, `--cast`) is missing entirely | `src/cast/{ssdp,soap,eventing,device_xml,transcode_server}.cpp`; `src/app/main.cpp:224-231`; `src/app/ui/shortcuts.cpp:210` | Missing | P1 | Add a casting entry to current status / features |
| A07 | README.zh.md | The shortcut list lacks `B` (media library overlay) | `src/app/ui/shortcuts.cpp:207` | Missing | P2 | Add `B` after `O` in the list |
| A08 | README.zh.md | The sentence "all can be rebound through soar/shortcuts.txt, moved with --shortcuts=, disabled with --no-shortcuts" is missing | `src/app/ui/shortcuts.cpp:445-454`; `docs/ui-design.md:108` | Missing | P1 | Append that sentence to the shortcut item (translate the same item from the English README) |
| A09 | README.zh.md | Usage examples lack `--hwdec=auto` / `--no-resume` / `--shortcuts=` / `--no-shortcuts` | `src/app/main.cpp:60-64,200` | Missing | P2 | Add four examples to the usage code block |
| A10 | README.zh.md | The test table lacks the `soar_dir_scan_tests`/`soar_library_tests`/`soar_shortcut_tests` rows | `tests/CMakeLists.txt:146,159,173` | Missing | P1 | Add three rows to the test table |
| A11 | README.md + README.zh.md | The test table lacks the `soar_cast_tests` row | `tests/CMakeLists.txt:119` | Missing | P2 | Add one row to each table |
| A12 | README.zh.md:155 | The roadmap v1.0 lists "casting (AirPlay/Chromecast/DLNA)" | No AirPlay/Chromecast code anywhere in the tree (the protocol choice is in the casting section of `todo.md`; DLNA was the only selection) | Ahead | P2 | Reword to "casting (DLNA/UPnP)"; move AirPlay/Chromecast out or mark them out of scope |
| A13 | README.zh.md:33 roadmap | "Media library and scraping" listed as v1.0 not started | Media library and offline NFO landed at `src/app/ui/library.cpp:190,396` | Mismatch | P1 | Reword to "media library and offline NFO landed; network scraping and multi-device sync TBD" |
| A14 | README.md | "Demux and decode run on separate threads" | The single decode thread is `src/core/ffmpeg_backend.cpp:757`; `av_read_frame` and decoding run on that same thread (`:2792`) | Mismatch | P2 | Reword to "demux and decode run on a dedicated decode thread, so the playback/UI thread is not blocked by IO" |
| A15 | README.md | "four event categories — state changes, progress updates, media info changes, errors" | `include/soar/core/backend.h:63-79` actually has 6 categories (plus BufferingStarted/Ended, DownloadProgress) | Mismatch | P2 | Reword to "four core event categories (plus buffering and download-progress events)" |
| A16 | README.md | "Backends are pluggable: FFmpeg, system frameworks, and fake backends" | `src/core/` contains only `ffmpeg_backend.cpp` + `null_backend.cpp`; "system frameworks" appears only here in the whole tree | Ahead | P2 | Delete "system frameworks" or reword to "(system framework backend reserved)" |
| A17 | README.md | mermaid diagram "Core library soar_core (pure abstraction, Apache-2.0 core)" | `CMakeLists.txt:60-66`: when FFmpeg is detected, `ffmpeg_backend.cpp` is compiled into `soar_core` with a PUBLIC link on `PkgConfig::FFMPEG` | Mismatch | P2 | Reword to "core library soar_core (interface is pure abstraction; the FFmpeg backend is compiled in when detected)" |
| A18 | README.md | "the CLI `--cast` flag" | `src/app/main.cpp:224-227` only accepts `--cast=<location\|auto>`; a bare `--cast` falls into path parsing | Mismatch | P1 | Reword to `--cast=<location\|auto>` |
| A19 | README.md | The shortcut list lacks `t` (casting overlay) | `src/app/ui/shortcuts.cpp:210` | Missing | P2 | Add `t` to the list |
| A20 | README.md | "torrent bridge URLs are skipped (their local port changes every session)" | What is skipped is the raw magnet/.torrent URI (`src/app/ui/player_window.cpp:478-481`); bridge URLs are actually recorded by `recordOpen` (`:810`) | Mismatch | P1 | Reword to "raw magnet/.torrent URIs are not added to history; bridge URLs are recorded (the port changes every session)" |
| A21 | README.md | "docs/mvp.md §3, docs/todo.md" | The repository root has `todo.md`; there is no `docs/todo.md` | Mismatch | P2 | Reword to `todo.md` |
| A22 | coverage-notes.md:24-31 and others | Several places say "local cov4 full run" | The six trees are system/cov3/tsan/tsan-o0/asan/noimgui (`soar_sixtree.sh:10`); there is no build-cov4 | Mismatch | P1 | cov4→cov3 |
| A23 | coverage-notes.md §3.6 | player_window.cpp line anchors (DROPFILE 359, toast 427, font fallback 206, etc.) | The current file is 3131 lines: DROPFILE `player_window.cpp:757`, toast `:1189-1251`, font fallback `:398/:428`, etc. | Mismatch | P1 | Re-anchor the §3.6 line numbers to the current file |
| A24 | coverage-notes.md §3.7 | http_cache.cpp line table (407/410, 606-608, etc.) | Currently 705 lines; `kRecvTimeoutSec=60` is at `:38` ✓ | Mismatch | P1 | Re-anchor the §3.7 line numbers to the 705-line file |
| A25 | coverage-notes.md §3.9 | "test_backend_media.cpp:1051" | Actually `tests/test_backend_media.cpp:1741` | Mismatch | P2 | Fix the line number |
| A26 | coverage-notes.md §3.13/§3.22 | ass_dialogue.cpp 136/137 | Currently 281 lines | Mismatch | P2 | Mark as batch-time readings or refresh |
| A27 | coverage-notes.md §3.15/§3.17 | Per-file denominators player_window.cpp 1090/1147, ffmpeg_backend.cpp 1712/1866 | Currently 3131 / 3763 lines | Mismatch | P1 | Mark as batch-time readings or refresh |
| A28 | coverage-notes.md §3.19 | "soar_p2p_tests (tests/test_p2p.cpp, 20 cases)" | `tests/test_p2p.cpp` actually has 21 TEST_CASEs | Mismatch | P1 | 20→21 |
| A29 | coverage-notes.md §3.23 | "cast suite 57 cases" | 58 cases after `f1b245d` | Mismatch | P2 | 57→58 |
| A30 | coverage-notes.md §1 | "on hermes PATH it resolves to pip's gcovr 8.6 by default"; "CI's apt is 7.0 (ubuntu noble)" | Locally `command -v gcovr` = /usr/bin/gcovr 7.2; the hermes environment could not be verified offline; `ci.yml` uses an unpinned ubuntu-latest matrix | Mismatch | P2 | Reword to "run `command -v gcovr` to check before running the gate"; verify the CI image claim against the current runner |
| A31 | alignment-audit-2026-10-13.md §1 difference 3 | "OSC has no A-B/CC/screenshot/prev/next buttons; no remaining time, no quality pill" | True at HEAD (no such buttons); the OSC batch (uncommitted in the worktree) already added all of those buttons plus remaining time | Mismatch | P1 | Note in the table header: that row closes once the OSC batch lands this round |
| A32 | alignment-audit-2026-10-13.md §1 | "mouse movement does not count as input (SDL_MOUSEMOTION not handled)" | True at HEAD; the OSC batch already added `case SDL_MOUSEMOTION` | Mismatch | P1 | Same header note |
| A33 | alignment-audit-2026-10-13.md §5 | "No Translate overlay, no shortcut" | `shortcuts.h:116-141` really has no Translate ✓; but the environment-gated `[translate]` menu row and `translateTextAndLoad` already exist (`player_window.cpp:490-513,1189-1251`) | Missing | P2 | Add a note on the partial implementation (panel not done; the menu row and translation pipeline exist) |
| A34 | alignment-audit-2026-10-13.md §1 | "toast (Paused)" | There is no Toast; it is a "Paused" chip (`player_window.cpp:2122-2130` drawChips) | Mismatch | P2 | Reword to chip |
| A35 | alignment-audit-2026-10-13.md §2 | "current row highlight (≻ marker + highlighted row)" | The marker is `"> "` (`player_window.cpp:2554`), not "≻" | Mismatch | P2 | Reword to `> ` |
| A36 | alignment-audit-2026-10-13.md §4 | "Subtitle track selection is in the 'Subtitle Settings' overlay (OSC Sub button)" | Track selection is the OSC Sub combo (`drawTrackCombo` `:1960`, call site `:1925`); "Subtitle Settings" (`:2725`) is the size/offset/visibility overlay | Mismatch | P2 | Distinguish the two |
| A37 | ui-design.md:100 | §3.1 "hidden ──(any key / mouse move / wheel / click)──▶ visible" includes mouse move | HEAD has no SDL_MOUSEMOTION handling; the OSC batch already added it | Ahead | P1 | Note in the table header: "mouse move" holds once the OSC batch lands this round |
| A38 | ui-design.md:110-133 | The §3.2 table says "the table above is the factory default" but has no `t` row | `src/app/ui/shortcuts.cpp` addDefault(Cast, kKeyT); overlay at `player_window.cpp:1477` | Missing | P1 | Add a `t` casting-overlay row to the table |
| A39 | ui-design.md:72-83 | The §2 overlay tree has no casting overlay entry | `src/app/ui/player_window.cpp:1477` drawCastOverlay + the t key binding | Missing | P1 | Add "casting overlay (t key)" to the §2 tree |
| A40 | ui-design.md:182 | "OSC height ~52px" | `src/app/ui/player_window.cpp:1835` bar_h=80.0f | Mismatch | P2 | Change to ~80px |
| A41 | ui-design.md:182 | "Overlay max width 720px" | Info overlay max 480 (`player_window.cpp:2263`, `:2724`); OSC bar max 920 (`:1834`); 720 appears nowhere in the tree | Mismatch | P2 | Change to 480px |
| A42 | ui-design.md:179 | "Panel rgba(16,18,24,0.92)" | `player_window.cpp:335` WindowBg alpha 0.94 | Mismatch | P2 | Change to 0.94 |
| A43 | ui-design.md:69 | "Buffering indicator (top-left '缓冲中…')" | `player_window.cpp:2089` chip text "Buffering..." | Mismatch | P2 | Change to "Buffering..." |
| A44 | ui-design.md:69 | "Download indicator (top-left '下载中 N%')" | `player_window.cpp:2097` "Downloading N%" | Mismatch | P2 | Change to "Downloading N%" |
| A45 | ui-design.md:79,130 | "Media library overlay (B key, v0.3)" | Landed: `shortcuts.cpp` Library→B; `player_window.cpp:2546` drawLibraryOverlay | Mismatch | P2 | Drop "v0.3" or note "landed" |
| A46 | mvp.md:37 | State change events: opening/playing/paused/stopped/ended/error | `include/soar/core/backend.h:54-60` PlaybackState has no Opening (a failed open surfaces as an Error event) | Mismatch | P2 | Drop "opening/" |
| A47 | mvp.md:25 | Playback rates (0.5x/1.0x/1.5x/2.0x) | `src/app/ui/player_window.cpp:1945` kRates={0.5,0.75,1.0,1.25,1.5,2.0} | Mismatch | P2 | Reword to "0.5–2.0 in six steps" |
| A48 | mvp.md:83 | soar_p2p_tests 20 cases | `tests/test_p2p.cpp` 21 TEST_CASEs | Mismatch | P2 | Change to 21 |
| A49 | ui-design.md:229 | "The backend itself requires 2.0.17 (SDL_RenderGeometry)" | The comment at `CMakeLists.txt:235-236` also says 2.0.17; SDL_RenderGeometry actually landed in SDL 2.0.18 | Mismatch | P2 | Change to 2.0.18 in both the doc and the CMake comment |
| A50 | Public surfaces | Description "built with FFmpeg, SDL2 and libass (C++17)", homepage, 13 topics | All accurate: `vcpkg.json`, `docs/.vitepress/config.mts:12` (base '/soar/'), `.github/workflows/deploy-docs.yml`, `src/core/http_cache.cpp`, `CMakeLists.txt:13` (SOAR_ENABLE_IMGUI optional) | — | — | Verified consistent, no fix needed |

## Verified consistent (spot checks, no fix needed)

- README's 31 factory-default shortcuts, `--cache-dir=`/`--torrent-*=`/`--no-resume`/`--shortcuts=`/`--no-shortcuts`/`--gui`/`--headless`/`--backend=`, and the environment variables (`SOAR_SUBTITLE_ENDPOINT/_API_KEY`, `SOAR_TRANSLATE_ENDPOINT/_API_KEY`, `SOAR_INSTALL_DIR`, `SOAR_TEST_X11`).
- glibc≥2.38 / GLIBCXX_3.4.32, package names (zip/deb/rpm/AppImage/`soar-setup-windows-x64.exe`/`SHA256SUMS.txt`), install directory, `xattr -cr`, `screenshot.png`, MRU×15, OSC 2.5s + four pinning conditions, Loop{Off,All,One}, Shuffle, the three-step A-B, the subtitle decode gate, `resumePositionMs` 5s/5s, libtorrent v2.0.15, the four FFmpeg libraries, CMake 3.20, SDL2≥2.0.18, 13 test suites, gcovr's directory-form `--filter`, Apache-2.0, HLS/DASH/RTSP smoke tests, libass MIT, the `soarw.exe` WIN32 target, SSDP/SOAP/GENA and transcode as H.264+AAC MPEG-TS, and the fact that the plugin system has not started.
- The alignment audit's OSC auto-hide (`kHideDelay=2500` at `ui_state.h:46`, the four pinning conditions at `ui_state.cpp:91-101`, the 0.16s fade at `player_window.cpp:835`), the settings page not implemented, the translate panel not implemented, all 7 evidence PNGs present, and todo.md at 28 checked + 7 unchecked.

## Increment (2026-11-02, batch G5)

| ID | Page | Claim | Source evidence | Category | Severity | Fix |
|---|---|---|---|---|---|---|
| G1 | ui-design.md §7 + zh twin | "Still not doing: … seek hover thumbnails … a settings panel" | Both landed 2026-11-02 (`0d07d89`): settings page (G key, `SettingsTab` `player_window.cpp:198`, `drawSettingsOverlay` `:2299`), seek thumbnails (ThumbnailService `:747` + `drawThumbPreview` `:2968`), library Type filter (`matchesSource` `:4346`/`:4369`) | code ahead of doc | P1 | §7 "landed" list extended with the three items (en+zh); "still not doing" reduced to file dialogs / mini mode / themes |
| G2 | ui-design.md §2 + zh twin | "v0.1 has no settings page … v0.2 turns them into a panel" | G1 evidence; panel shipped | code ahead of doc | P1 | Reworded to "settings panel landed 2026-11-02 (G key)" in both languages |
| G3 | prototype-alignment-audit.md M4/M7 + zh twin | "None — Intentionally deferred (v0.2)" | G1 evidence | code ahead of doc | P2 | Rows annotated "landed 2026-11-02 (`0d07d89`)" with code anchors (historical verdicts kept) |

Three-bucket count for this increment: code ahead of doc **3** (G1–G3) · doc ahead of
code **0** · description mismatch **0**. P0: none. All fixed docs-only; the finding was
first registered in `todo.md` by the 2026-11-02 review pass and is closed by this round
(row ticking left to that ledger's owner).

Copy-style fixes (same round): README.md track-selection bullet "seamless while playing
or paused" → "works while playing or paused, without interrupting playback";
`docs/zh/coverage-notes.md` ⭐ marker replaced with a bold lead-in and the "（必读…）"
heading softened. Historical snapshot sections (alignment-audit-2026-10-13.md,
prototypes.md audit narrative) left as dated records per the closeout convention.


## Increment G2 (2026-11-14, fleet docs audit)

Scope: the six high-drift surfaces — feature list, CLI flags, shortcut table,
platform/artifact matrix, suite table, badges/About — audited against the code
at `9552d62`, both README editions and the docs site. Severity per the standard:
doc ahead of code = P0, artifact/manifest drift = P1, wording/links = P2.

| ID | Page | Claim | Source evidence | Category | Severity | Fix |
|---|---|---|---|---|---|---|
| G2-1 | README.md features, README.zh.md:38 | Casting bullet absent from the English features list (the zh twin had one); `--cast-iface=` and `--open-dialog` accepted by the CLI but documented nowhere | Both landed in `599ec4e`; parsed at `src/app/main.cpp:257` and `:264`, printed by `--help` at `:66` and `:84`, consumed at `player_window.cpp:821` | code ahead of doc | P1 | En features list gained a **Casting** bullet (mirroring the zh one) carrying `--cast-iface=`; `--open-dialog` documented in the mouse/keyboard bullet of both editions |
| G2-2 | README.md, README.zh.md features | Settings overlay (`G`) and the light/dark Appearance row (`y`) absent from both features lists | `OpenSettings`→`kKeyG` `src/app/ui/shortcuts.cpp:202`, `ToggleTheme`→`kKeyY` `:200`, `ShortcutAction::ToggleTheme` `shortcuts.h:142`; five tabs at `player_hud_settings.cpp:467-487` (General/Playback/Subtitles/Network & Download/About), Appearance card `:588` with the Dark/Light segmented control `:260`; `loadThemePref` `player_window.cpp:1077` | code ahead of doc | P1 | **Settings overlay** bullet added to both editions, listing the five tabs and the persisted Appearance choice |
| G2-3 | README.md:41, README.zh.md:42 shortcut lines | `G`, `y`, `/` and `Ctrl+O` absent from the shortcut lists | `shortcuts.cpp:198` (`SearchLibrary`→`kKeySlash`), `:200` (`ToggleTheme`→`kKeyY`), `:202` (`OpenSettings`→`kKeyG`), `:188` (`OpenFolder`→`kKeyO` with Ctrl); `/` handling at `player_window.cpp:656` | code ahead of doc | P1 | All four keys added to both editions' shortcut lines |
| G2-4 | README.md, README.zh.md suite tables | En table listed 13 suites, zh 13 — the build registers 15 | `tests/CMakeLists.txt:117` (`soar_p2p_tests`), `:82` (`soar_metadata_tests`), `:206` (`soar_uri_tests`); the en table had `soar_metadata_tests` but not `soar_p2p_tests`, the zh table the opposite | code ahead of doc | P1 | Both tables now carry all 15 registered suites; the new rows describe the suites from their CMake headers (self-seeding in-process P2P, forked fixture HTTP server for metadata, `file://` decoding for uri) |
| G2-5 | README.md, README.zh.md | The iOS slice (`ios/SoarKit`, `ios/Bridge/soar_bridge.h`, `ios.yml`) appears nowhere in either README | `ios/SoarKit/{Package.swift,Sources,Tests}`, `xcodebuild` build + test in `.github/workflows/ios.yml:22-33` on `ios/**` pushes, the status table in `ios/README.md` (SoarKit CI-verified; bridge un-wired; core cross-compile and the app shell not started) | code ahead of doc | P1 | **iOS slice** bullet added to both editions' roadmap, stating the CI-verified scope and the two not-started parts |
| G2-6 | README.zh.md links | Four zh link targets `docs/{build,mvp,licensing,coverage-notes}.zh.md` never existed (8 occurrences: 4 links + 4 bare mentions) | `f0f2c33` moved zh pages into `docs/zh/`; `ls docs/zh/` resolves all four | description mismatch (stale pointer) | P2 | All 8 repointed to `docs/zh/<name>.md`; every target verified to resolve |
| G2-7 | docs/prototypes.md:63, docs/zh/prototypes.md:67 | "2 screens not implemented (settings panel, translate overlay)" | Settings panel shipped 2026-11-02 (`0d07d89`); the translate panel was retired by the v2 contract, its entry moving to the `[translate]` row of the subtitle menu (`todo.md:180`) | description mismatch | P2 | Annotated in place with the current state; the dated verdict kept as a historical record, matching the G3 convention |

Copy/pointer fixes (same round): the dead `docs/ui-design.md §3.2` citation in
README.md:40 dropped (P2, the file went away in `58b9b57` on 2026-11-11); the
same citation in README.zh.md dropped; the `ui-design §2` phrase in
`docs/prototypes.md` is retained as a historical attribution (it records what the
superseded spec said) rather than as a pointer.

Bucket and severity differ here, and deliberately: a dead pointer is a link fix,
and a stale verdict is a wording fix, both of which the standard maps to P2 —
they are not ghost features and not artifact drift. Three-bucket count for this
increment: doc ahead of code **0** · code ahead of doc **5** (G2-1…G2-5) ·
description mismatch **2** (G2-6, G2-7). Severity: P0 **0**, P1 **5**, P2 **4**
(G2-6, G2-7 and the two dead `ui-design.md` citations below). P0: none — every
feature the docs claim was verified present (`--cast-transcode`
`src/app/main.cpp:195,255`; the plugin system is genuinely absent; the nightly
artifact list matches `nightly.yml:522-540` and README:112; presets and the
platform boundaries at README:107/111 match the workflow matrix). Docs-only
changes; no code was moved to fit the text.

### Browser language detection (same round)

Form: paired files (`README.md`/`README.zh.md`) **and** vitepress locale routing —
`docs/.vitepress/config.mts:37-40` declares `locales.root` with `lang: 'en'`
(English at the site root) and `:86` `lang: 'zh-CN'` for `zh/`, under
`base: '/soar/'`; 12 English pages against 12 Chinese pages, exact parity.

Auto-detection: **present, on the root landing page only**. `docs/index.md`
carries a `<script setup lang="ts">` block that, behind a root-path guard
(`p !== ROOT_PATH && p !== ROOT_PATH.slice(0, -1)` returns early — the script
never acts outside `/soar/`), checks `localStorage` first, then
`navigator.languages`/`navigator.language`, and replaces the URL with
`/soar/zh/` when any locale starts with `zh`; a capture-phase click listener on
the same page records manual choices so a later auto-check yields. The zh
landing page carries **no** mirror script (`grep script docs/zh/index.md` is
empty; `soar-lang-auto-picked` appears only in `docs/index.md` site-wide), so a
browser landing directly on `/zh/` is never auto-switched back — filed as a P2
todo (mirroring the script or folding detection into the theme waits for the
user). Corrected 2026-11-15: an earlier version of this paragraph said both
landing pages carry the script; only the root page does.

Live URL contract probe (fetched 2026-11-14):

| URL | HTTP | `<html lang>` | first-screen CJK |
|---|---|---|---|
| `https://cuihairu.github.io/soar/` | 200 | `en` | 223 |
| `https://cuihairu.github.io/soar/zh/` | 200 | `zh-CN` | 447 |

The root serving `en` is correct and matches the local config — the 223 CJK
characters on the English landing page are the language-switcher label and the
prototype-preview nav names, not a stale deployment. The fleet-level note that
this repo's root serves Chinese was not reproduced; no redeploy is needed and no
config change was made. No todo: the auto-switch enhancement already exists here.

## Increment G2 re-audit (2026-11-15)

Same fleet round, re-run against `eda4ddc`. Scope: the six high-drift surfaces
(feature list, CLI flag tables, platform/artifact matrix, install/build,
README badges and the GitHub About line, nightly artifact lists) plus a full
mechanical re-verification of every `file:line` citation in the live doc pages
— the last round verified the *content* of the citations but not whether the
lines still existed. Baseline for this increment: `eda4ddc`; the tree was clean
and matched `origin/main` at start.

| ID | Page | Claim | Source evidence | Class | Severity | Fix |
|---|---|---|---|---|---|---|
| G2R-1 | docs/index.md:28, docs/zh/index.md:28, README.zh.md:18 | "FFmpeg, a system framework, or the fake backend … implement the same IBackend interface" (en hero card, zh hero card, zh README) | `IBackend` has exactly two implementations in the tree: `src/core/ffmpeg_backend*.cpp` and `src/core/null_backend.cpp`; `ls src/core/` shows no other backend file, and `grep -riE 'avfoundation\|media3\|exoplayer' src/ include/ tools/` is empty. `docs/architecture.md:28` already says "two implementations" | doc ahead of code | **P0** | Reworded all three to "FFmpeg and the fake backend for tests implement the same `IBackend`". `docs/mvp.md:10` ("prefer a system framework") and `docs/licensing.md:79` (`backend_avfoundation`) were **left alone** — both are stated as forward-looking design guidance, not as shipped code |
| G2R-2 | docs/architecture.md (6), docs/glossary.md (4), docs/rationale.md (4) + the zh twins | 24 `file:line` citations pointed past the end of their target files | The UI monolith was split on 2026-11-10 (`51ae7fe`, *refactor(ui): split PlayerHud into header and per-concern files*): `player_window.cpp` is now 1193 lines but docs cited 5099/5205/2035/2115; the FFmpeg backend was split the same way (916 lines) but docs cited `ffmpeg_backend.cpp:3005`/`:2576`; `subtitle_provider.cpp` is 474 lines but docs cited 609. Every citation now resolves: `runPlayerWindow` → `player_window.cpp:1019`, bare no-ImGui loop → `:1142`, `PlayerHud` ctor → `:278`, `handleEvent` → `:504`, HUD dtor → `:419`, theme namespace → `player_hud.h:133`, geometry tokens → `ui_tokens.h:19`, `theme.txt`/`settings.txt` → `player_hud.h:311`/`:348`, `SOAR_UI_BITMAP_FONT` → `player_hud.h:478`, endpoint env reads → `player_window.cpp:335/354/378`, cast discovery/per-frame/overlay → `player_hud_actions.cpp:502`/`:520`/`:590`, `decodeLoop` → `ffmpeg_backend_decode.cpp:15` (spawned from `ffmpeg_backend.cpp:417`), `HttpCache` construction → `ffmpeg_backend_open.cpp:161`, `storeExternalSubtitle` → `subtitle_provider.cpp:424`, `CastSession::start` → `cast_session.cpp:40` | description mismatch | P1 | All 24 re-anchored in both languages; a script re-verifies that every citation resolves — 0 stale |
| G2R-3 | README.zh.md:29, :38, :161 | The zh edition had gone behind the en one on three landed features | Network scraping (`src/core/metadata_scraper.cpp`, `HttpMetadataScraper` wired through `MetadataSource`; env `SOAR_META_ENDPOINT`/`_API_KEY`/`_TIMEOUT_MS` read at `player_window.cpp:378`) was in the en feature bullet and absent from zh; `--cast-transcode` (`src/app/main.cpp:195`, printed by `--help` at `:81`) was in the en casting bullet and absent from zh; the v1.0 casting status (SSDP/SOAP/GENA, pause, bridge LAN rebind, transcode, `t` overlay, real-device matrix unverified) was a single clause "投屏（DLNA/UPnP）" in zh against the full en paragraph | code ahead of doc | P1 | zh feature/roadmap bullets brought to parity with the en ones |
| G2R-4 | README.md:113, README.zh.md:113 | The download section listed the per-platform artifacts but not the aggregate bundle or its two text files | `gh release view nightly --repo cuihairu/soar --json assets` returns 15 assets including `soar-daily-20261114-9552d62f.zip` (787 MB), `BUILD_INFO.txt` and `VERIFY.md`; produced at `.github/workflows/nightly.yml:803,745,746` | code ahead of doc | P1 | Both editions now name the aggregate bundle and the two text files |
| G2R-5 | README.md:195-196 | `soar_p2p_tests` appeared twice in the en suite table (16 rows, 15 unique) while the zh table had 15 | `tests/CMakeLists.txt` registers each suite once; the zh table already matched | description mismatch | P2 | Duplicate row removed; both tables now carry the same 15 suites |
| G2R-6 | README.zh.md:50, :55 | Mermaid node labels drifted from the en diagram | `README.md:50` lists "info/recent/help/playlist/library overlays" and `README.md:55` says "pure abstraction; FFmpeg backend compiled in when detected" (`CMakeLists.txt:63-74` compiles the FFmpeg backend into `soar_core` when detected); the zh twins said "信息/最近/帮助浮层" (three overlays) and "纯抽象，Apache-2.0 本体" | description mismatch | P2 | Both zh labels aligned with the en ones |
| G2R-7 | docs/audit-doc-consistency.md:186-188 (the previous round's own table) | The live probe table mislabelled its CJK column | `https://cuihairu.github.io/soar/` is 19 109 bytes with its **first** CJK character at byte 4021 (the `简体中文` switcher label), so "first-screen CJK = 223" was really the whole-page count and the true first-2000-byte count is 0; `/soar/zh/` is 447 whole-page / 18 first-2000-bytes (the `<meta name="description">` at byte 216) | description mismatch | P2 | The probe triple is re-recorded below with the whole-page and first-screen columns kept apart |

Three-bucket count for this increment: doc ahead of code **1** (G2R-1) ·
code ahead of doc **2** (G2R-3, G2R-4) · description mismatch **4**
(G2R-2, G2R-5, G2R-6, G2R-7). Severity: P0 **1**, P1 **3**, P2 **3**. All
fixes were docs-only; no code was moved to fit the text.

Verified consistent this round (spot checks, no fix needed): the feature and
shortcut lists against `src/app/ui/shortcuts.cpp`; every CLI flag in
`README.md:129-155` against `src/app/main.cpp:66-97` (`--open-dialog`,
`--hwdec=`, `--cast-transcode`, `--cast-iface=` all parse) and against
`--help`; the environment variables `SOAR_SUBTITLE_ENDPOINT`/
`SOAR_TRANSLATE_ENDPOINT`/`SOAR_META_ENDPOINT` at
`player_window.cpp:335/354/378`; the OSC 2.5 s auto-hide and MRU×15 claims at
`ui_state.h:46` and `:205`; the 256 KiB cache block at
`http_cache.cpp:32`; `packaging/linux/soar.desktop.in:6` (`Exec=soar --gui %U`)
and `:10` (the four MimeType entries); `SOAR_BUILD_APP`/`SOAR_BUILD_TESTS` at
`CMakeLists.txt:9,10`; the nightly artifact matrix against
`.github/workflows/nightly.yml:792-803` (14 per-platform artifacts + the
aggregate, matching the 15 release assets above); and the GitHub About line
("Cross-platform open source media player built with FFmpeg, SDL2 and libass
(C++17)") against `vcpkg.json` and the built targets. No dead internal doc
links: every `[text](/page)` and `[text](./file.md)` in the audited pages
resolves to a real page or file, and the two links from `docs/prototypes.md`
to `/alignment-audit-2026-10-13` and `/prototype-alignment-audit` resolve to
pages that exist even though they are not in the sidebar (not unlinked — they
are reachable and intentional).

### Live URL contract probe (re-run 2026-11-15)

| URL | HTTP | `<html lang>` | CJK, whole page | CJK, first 2000 bytes |
|---|---|---|---|---|
| `https://cuihairu.github.io/soar/` | 200 | `en` | 223 | **0** (first CJK byte at 4021 — the `简体中文` switcher label) |
| `https://cuihairu.github.io/soar/zh/` | 200 | `zh-CN` | 447 | **18** (the `<meta name="description">` at byte 216) |

The previous round's table reported 223 and 447 under a "first-screen" header;
both numbers are whole-page counts. The corrected reading changes nothing about
the verdict (the root correctly serves `en`), only about the label.
