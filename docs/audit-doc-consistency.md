
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
