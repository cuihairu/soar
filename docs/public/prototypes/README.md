# Prototype delivery directory

The prototype track of the homepage **Interface Preview** carousel
(`docs/.vitepress/theme/Screenshots.vue`) loads images from this directory by the
**fixed filenames** below. Until a file arrives, the carousel renders a placeholder
card; after the matching file is added and the site is next built and deployed, the
real image appears automatically without component changes.

| Filename | Group | Content | Status |
|---|---|---|---|
| `redesign/desktop-main-dark.png` | Playback | Desktop main window (final v2) | ✅ Delivered (v2.1 review, 2026-10-07). ⚠️ Not re-captured after the top bar was removed from the main-window page on 2026-10-15 (this repository has no browser rendering environment): the screenshot remains the final frame with the top bar and awaits recapture when an environment is available. |
| `redesign/playlist-dark.png` | Playback | Playlist (final v2) | ✅ Delivered |
| `redesign/library-dark.png` | Playback | Media library (final v2) | ✅ Delivered |
| `redesign/settings-dark.png` | Settings | Settings panel (final v2) | ✅ Delivered |
| `mobile.png` | Mobile | Mobile interface | ⬚ Not delivered; keep the placeholder card (mobile is outside the desktop batch) |

Prototype cards always use the **final v2** screenshots (the `redesign/`
subdirectory; the dark set is the definitive set). Since 2026-10-15, the three v1
PNGs (`desktop-main.png`, `settings.png`, and `translate-panel.png`) are no longer
referenced by the carousel. They remain in the repository because the v1 pages are
still present and the **legacy comparison** links on each v2 page point to them,
but the homepage no longer displays them. The `translate-panel` card was removed:
v2 did not redesign that page, and translation in the implementation is reached
through the `[translate]` row in the subtitle menu and will be covered by a real
product screenshot.

Requirements: PNG (or change the extension in `Screenshots.vue` directly),
preferably 16:9 (for example 960×540 or 1920×1080), and displayed in the same
frame as real product screenshots.

## Five desktop prototype pages (2026-10-07 batch)

The five HTML mockups—`desktop-main`, `playlist`, `settings`, `library`, and
`translate-panel`—were designed in this repository for a 1280×800 desktop viewport
and share a visual language defined in `assets/proto.css`. For the overview and
per-page design notes (layout, interaction, and state), see `index.html` in this
directory (`/prototypes/index.html` after site deployment). This file and the PNG
files are copied verbatim as static assets and do not enter VitePress page routing,
so there are no in-site links here.

Since 2026-10-15, these five v1 pages have carried an **v1 legacy (deprecated)**
header, with the v2 redesign available under `redesign/`. v2 supersedes their
visual language and information architecture. The sole remaining use of v1 is the
**legacy comparison** link on each v2 page. The v1 versions of `playlist.png` and
`library.png` are outside the carousel contract but remain available for review
and documentation references; the carousel uses their **final v2** screenshots
(see the table above).

For transparency: the brand slot is a neutral placeholder (an indicative “S”
glyph, not a logo design). The media library is a forward-looking design for the
v1.0 roadmap and has not been implemented; the other four pages correspond to
capabilities that have landed. The mockups are not product code; their notes and
`kbd` labels are review annotations.

To replace them later with external designs (Figma, Sketch, or hand sketches all
work), deliver files under the fixed filenames above—no component change is needed.
