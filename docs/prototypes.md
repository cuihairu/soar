[English](/prototypes) | [中文](/zh/prototypes)

# Prototypes

> Desktop player v2 redesign: four pages — main player window / playlist / settings panel / media library — in light and dark themes.
> What follows is a **design mock** (HTML/CSS prototypes, the static pages under `docs/public/prototypes/`), not product code.
> Where the implementation lands: see the [UI spec](/ui-design). The brand mark is a neutral placeholder (solid orange block plus an S), not the official logo.

## Live preview

Every page accepts a `?theme=light|dark` parameter or an in-page button to switch themes. Screenshots are 1280×800 static frames.

| Page | Interactive prototype | Dark screenshot | Light screenshot |
| --- | --- | --- | --- |
| Main player window | [Open](/prototypes/redesign/desktop-main.html) | [Dark](/prototypes/redesign/desktop-main-dark.png) | [Light](/prototypes/redesign/desktop-main-light.png) |
| Playlist | [Open](/prototypes/redesign/playlist.html) | [Dark](/prototypes/redesign/playlist-dark.png) | [Light](/prototypes/redesign/playlist-light.png) |
| Settings panel | [Open](/prototypes/redesign/settings.html) | [Dark](/prototypes/redesign/settings-dark.png) | [Light](/prototypes/redesign/settings-light.png) |
| Media library | [Open](/prototypes/redesign/library.html) | [Dark](/prototypes/redesign/library-dark.png) | [Light](/prototypes/redesign/library-light.png) |

Full rationale (the list of what was wrong with the older draft, and per-page reasons for each change) in the [redesign overview](/prototypes/redesign/index.html).

## Key frames

### Main player window (light)

<img src="/prototypes/redesign/desktop-main-light.png" alt="Main player window, light" width="60%">

The shell (status chip / control bar / annotation overlay / progress groove) uses light colors in this theme; the video canvas and the subtitles on it are the content layer and stay black in both themes.

> ⚠️ Both main-window screenshots were taken from the 2026-10-07 final frame and **include a top bar**; the 2026-10-15 patrol round removed the top bar from the v2 draft (no top bar is the v0.1 baseline — see the [UI spec](/ui-design) §2). This repository has no browser rendering environment, so the frames were not retaken; that one known difference between the screenshots and the current prototype pages is retained until they are.

### Main player window (dark)

<img src="/prototypes/redesign/desktop-main-dark.png" alt="Main player window, dark" width="60%">

### Settings panel (light)

<img src="/prototypes/redesign/settings-light.png" alt="Settings panel, light" width="60%">

### Media library (light)

<img src="/prototypes/redesign/library-light.png" alt="Media library, light" width="60%">

### Playlist (light)

<img src="/prototypes/redesign/playlist-light.png" alt="Playlist, light" width="60%">

## Decision log

### v2.1 review sign-off (2026-10-07)

- **Dark set of four pages passed, finalized.**
- **Light set sent back for rework (completed in this revision)**: the v2 light version was not what it claimed — the shell was still uniformly dark. v2.1 themes the whole shell: in the light theme the top bar, status chip and pill overlays, annotation overlay, OSC control bar and progress groove all use real light colors; the video canvas and the subtitles on it are the content layer and stay black in both themes (the normal shape for a real player, and not a case of "fake light").
- **Two small changes**: the main-window progress groove thickened from 4px to 6px (handle 10px to 12px, both themes in sync); the two annotation blocks top-left merged into one (tighter information density; the OSC auto-hide explanation is kept — one annotation should not blur into four blocks).
- **Review scope**: re-shoot the four light pages plus the dark main-window frame (progress groove in sync); the other three dark pages stay as drafted.

### v2 redesign scope (2026-10-06)

- redesign-preserve: structure and information architecture unchanged, visual language tightened (gradients removed, a single signal orange as the accent, sky blue reserved for network/P2P semantics only, 11px font-size floor, corner radii made explicit — 10 for panels, 7 for controls, pill for chips, circular for icon buttons, an infinite-animation bundle guarded by `prefers-reduced-motion`).
- Design tokens in three layers (primitive / semantic / component); components never read bare color values. Contrast checked against WCAG AA (metadata tier: 6.0:1 on dark, 4.8:1 on light).

## Alignment acceptance (2026-10-13)

Per-screen three-way verdicts and screenshot evidence in the [alignment acceptance report](/alignment-audit-2026-10-13): 3 screens with differences (main player window / playlist / media library), 2 screens not implemented (settings panel, translate overlay — ui-design §2 sets v0.1 as having no settings page). The 7 implementation differences were registered in the todo as the "prototype alignment acceptance batch"; the milestone marks on the prototype pages landed (2026-10-14, patrol round): `redesign/index.html` gained a "prototype-ahead-of-spec milestone marks" section (settings is a v0.2 planning draft; translate / top bar · status pill / OSC quality pill are v0.2+; library thumbnails · card grid · source chips are v0.3), and the `prototypes/translate-panel.html` page header is marked v0.2+.

## Docs-site consistency (closed 2026-10-15)

The "UI preview" cards on the home page used to point at **v1** PNGs (`prototypes/desktop-main.png` and the like), which contradicted this page linking only v2 — the same site claiming v2 was final in one place and showing v1 frames in another. This round closed it out:

- **Prototype cards now point at the finalized v2 screenshots**: `redesign/desktop-main-dark.png`, `redesign/settings-dark.png`, plus `redesign/playlist-dark.png` and `redesign/library-dark.png`, which had never appeared on the site (they were registered "deploy and they're live" — the files were already in the repo).
- **The translate-panel card was removed**: v2 did not re-draft that page (the translate feature in the implementation runs through the subtitle menu's `[translate]` row, which the two real screenshots in that group already cover).
- **The v1 prototype set is marked deprecated**: all six pages under `prototypes/` (including the `index.html` overview) get a "v1 old draft (deprecated, v2 is at …)" line in the doc bar linking to the corresponding v2 page; the v1 pages are kept, not deleted — v2's "old draft for comparison" links still point at them. The translate-panel page notes that v2 did not re-draft it.
- The mobile card (`mobile.png`) stays a placeholder: the mobile shell has no UI to align against yet (see [alignment audit](/prototype-alignment-audit) §6 for the per-platform split), and there is no v2 draft to point at.

## Top bar removed (patrol round 2026-10-15, proxy decision a)

The three items from alignment audit §5-P2-7 were proxy-decided by the user as **(a) keep no top bar**: the top bar of the four v2 main-player-window frames (brand mark + centered title + the four I/R/H/F tool buttons) was removed entirely, for the reasons the audit recommended — those entries live in OSC plus shortcuts (I/R/H/F are all in the §3.2 contract), the title goes through the OS window title, and adding a top bar introduces permanent chrome area that conflicts with the §1.6-1 research conclusion. **The top bar and the light theme are both v0.2 candidates, not v0.1 baseline** (synced in [UI spec](/ui-design) §2/§4). The two main-window screenshots are pre-removal final frames and were not retaken (no rendering environment); the known difference is noted above.
