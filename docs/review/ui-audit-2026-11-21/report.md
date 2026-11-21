# Native desktop UI audit — 2026-11-21

Vision-based audit of the shipped desktop window under the walkthrough's pinned
configuration (`SOAR_UI_BITMAP_FONT=1`, exclusive Xvfb, 960x540 SDL window).
Each state was driven by keyboard only (no Home, no Escape — both would end the
capture: a seek-to-start while paused drops to Stopped, and in-window Escape
quits), captured in both themes, and reviewed against the v2 redesign prototype
(`docs/public/prototypes/redesign/`, the design baseline the v2 work pins to).

Evidence: `shots/` (28 states x dark/light, plus verification crops).

## Findings and disposition

| # | Sev | Finding | Evidence | Disposition |
|---|-----|---------|----------|-------------|
| 1 | P1 | Settings "ESC exits fullscreen" description says "in-window ESC closes settings"; in-window ESC **quits** (`player_window.cpp:550-560` — fullscreen→window else `*quit=true`, no overlay close) | `shots/settings_light.png` | **Fixed** — copy corrected to "In fullscreen, ESC returns to the window; otherwise ESC quits" (`player_hud_settings.cpp:588`) |
| 2 | P1 | Left-rail / page section headers and tertiary text use `kText4`: light #98A1AF on #EEF0F4 = **2.3:1**, dark #4D5560 on #111318 = **2.2:1** — both below WCAG AA (4.5:1) | `shots/library_light.png`, `shots/settings_light.png` | **Fixed** — all 8 `kText4` usages mapped to the prototype tier (`kText3` #626C7D light / #6E7886 dark = 4.8:1, passes; body section headers to `kText2` per the prototype `.sect h2`). The `kText4` token (a 4th dim tier) is removed — the prototype defines **no** `--text-4` in either theme (proto.css:38-70): `.side .grp`, `.side a .cnt`, `.row .idx`, `.search .kbd`, `.ep .val.dim` are all `--text-3`; `.sect h2` is `--text-2`. `player_hud_pages.cpp` x5, `player_hud_settings.cpp` x4, `player_hud.h` x2 |
| 3 | P2 | Shortcuts/Help table (380x400 fixed) clips the last ~6 rows (Cast / Queue / Help / Leave fullscreen / Quit / Focus search); no scrollbar (`BeginTable` lacked `ScrollY`); 28 rows ≈ 582px cannot fit the 540px default window | `shots/help_dark.png` (before: rows end at "Playlist queue", last 6 clipped) | **Fixed** — `ImGuiTableFlags_ScrollY` added (`player_hud_overlays.cpp:244-249`); verified `shots/fix_help_dark.png` shows the scrollbar |
| 4 | P2 | Recent overlay fixed 460x340 — ~230px dead space with the seeded 2-3 rows | `shots/recent_dark.png` | **Reported, not fixed** — the walkthrough clicks recent rows at **absolute window-relative coords** `moved(480,158)/(480,177)` (`test_cli.cpp:668-679`, 19px pitch). Auto-fitting the window would move the rows and break those clicks. Recalibration is a separate pass |
| 5 | P2 | Cast overlay fixed 520x420 — dead space in the no-torrent 3-line state | `shots/cast_dark.png` | **Reported, not fixed** — the cast probe crops 520x420 centered (`test_cli.cpp:3695-3775`, invariant "discovered row = last band, Search again = third from bottom"). The walkthrough always opens Cast with a torrent (content-filled ≈420), so auto-fit is a no-op there, but the no-torrent shrink is unexercised by the probe and the invariant margin is untested |
| 6 | P3 | Subtitle combo preview truncates: "Sub 0/" (`player_hud_render.cpp:411`) | `shots/sub_zoom_3x.png` | **Reported** — cosmetic; the combo width leaves no room for the denominator |
| 7 | P3 | Library watched-path clips without an ellipsis | `shots/library_light.png` (the "Watched: 1" row) | **Reported** — cosmetic; no `clipEllipsis` on that path |
| 8 | P3 | Library search box has no focus indicator when empty (focus is visible only once text is typed) | `shots/search_light.png` | **Reported** — cosmetic; `blur_text_next_` already makes focus escapable via Tab (V7b) |

## Observation (not a defect)

The implementation's dark theme `text-2`/`text-3` (#A2ABB8 / #6E7886) are dimmer
than the prototype's dark scale (#B9C0CB / #8B94A3). Both pass AA (≥4.8:1), so
this is a dimness preference rather than a compliance failure; left unchanged so
the dark shell's look is not restyled by this round. The light theme tokens match
the prototype exactly.

## Fix summary

- `src/app/ui/player_hud_settings.cpp` — ESC copy; nav `.grp` → `kText3`;
  `settingsSect` → `kText2`; endpoint disabled value → `kText3`; endpoint note → `kText3`
- `src/app/ui/player_hud_pages.cpp` — playlist index → `kText3`; `librarySideGrp` → `kText3`;
  TYPE count → `kText3`; search "/" hint + icon → `kText3`
- `src/app/ui/player_hud_overlays.cpp` — Shortcuts table gains `ImGuiTableFlags_ScrollY`
- `src/app/ui/player_hud.h` — `kText4` removed from both theme token blocks
