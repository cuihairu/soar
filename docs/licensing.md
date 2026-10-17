
# Choosing and bounding multimedia dependencies in an Apache-2.0 project (not legal advice)

This document is written for "player / playback engine" projects: you want the project itself to stay Apache-2.0 while integrating third-party multimedia libraries on different platforms.

> Conclusion first:
> - **Permissive licenses (MIT/BSD/Apache/zlib/ISC and similar)**: usually usable as-is.
> - **LGPL**: usually usable, but only if you meet its obligations (relinking, replacing the library, providing licenses and source of modifications, and so on). Be especially careful about mobile targets and distribution forms where the library cannot be replaced.
> - **GPL**: if you distribute GPL code (library or plugin) together with your product as "one combined work", the distribution is normally pulled into GPL — which usually conflicts with the goal of keeping the project Apache-2.0.

## 1. The boundary you are trying to hold

To keep the repository / core code distributed under Apache-2.0 you normally need all of the following:

- **Do not distribute GPL dependencies** (including statically or dynamically linked GPL libraries, GPL plugins packaged with your build, or GPL code copied straight into the source tree).
- **Comply with LGPL for LGPL dependencies**: ship the license texts and notices; let users replace or relink the library (including the source of any modifications you made to LGPL components).
- **Separate "source license" from "codec patents / store terms"**: even where licenses are compatible, some codecs (H.264/HEVC/AAC and similar) may still raise patent-licensing or store-policy questions.

## 2. Licensing and usability of common libraries (for an Apache-2.0 project)

This is an experience-based table of "common choices" — what applies depends on **the build artifacts you actually use or ship**, and a single project may carry several licenses or build options.

### Demuxing / decoding / playback core

- **FFmpeg**: `LGPL-2.1-or-later` (default) / optionally `GPL-2.0-or-later`
  - Usability: usable (choose the **LGPL build**); avoid options and external libraries that pull in GPL.
  - Boundary: if the binary you package contains GPL components, the distribution is subject to GPL.
  - Reference: `https://ffmpeg.org/legal.html`

- **GStreamer**: core is usually `LGPL-2.1-or-later`; the plugin set is a license mix (LGPL/GPL/proprietary)
  - Usability: usable (pick the LGPL / license-compatible plugin set).
  - Boundary: **do not distribute GPL plugins** (unless you are willing to accept GPL for the whole distribution).
  - Reference: `https://gstreamer.freedesktop.org/documentation/additional/design/licensing.html`

- **libVLC / VLC SDK**: usually `LGPL-2.1-or-later` (but the components actually packaged may be mixed)
  - Usability: generally usable (integrate as LGPL components).
  - Boundary: check the licenses of the VLC modules, plugins and binaries you actually ship; do not fold GPL parts into your release.
  - Reference: `https://wiki.videolan.org/License/`

- **libmpv / mpv**: `GPL-2.0-or-later`
  - Usability: if you insist on a non-GPL distribution (Apache-2.0), shipping it as a bundled dependency is generally not recommended.
  - Reference: `https://github.com/mpv-player/mpv`

### Subtitles / rendering / auxiliary libraries (usually better compatibility)

- **SDL2**: `zlib` (permissive)
  - Usability: usable.
  - Reference: `https://github.com/libsdl-org/SDL`

- **Dear ImGui**: `MIT`
  - Usability: usable.
  - Reference: `https://github.com/ocornut/imgui`

- **libass (ASS/SSA subtitles)**: commonly `ISC`
  - Usability: usable.
  - Reference: `https://github.com/libass/libass`

> Note: even where a library itself is permissively licensed, its "optional dependencies / backend plugins" can still introduce GPL/LGPL/proprietary terms. Compliance is judged on the final packaged artifact.

## 3. Key LGPL compliance points (where players often trip)

LGPL versions and specific situations have many details; here is an engineer-actionable checklist:

- **Prefer dynamic linking**: ship LGPL libraries as replaceable dynamic libraries / shared objects wherever you can.
- **Bundling LGPL .so/.dylib/.dll with the release is also compliant** (the approach this repository uses for its Linux/macOS/Windows packages): copy from the distribution or vcpkg unchanged, keep it unmodified and replaceable — the user can delete `lib/` (or the co-located DLLs) and substitute their own build. The obligations are the same as the previous bullet.
- **Allow relinking / replacement**: the user must be able to replace the LGPL library under reasonable conditions (for static-linking scenarios, provide relocatable object files / relinking scripts).
- **Provide licenses and notices**: include the LGPL text, copyright notices and third-party statements in the release.
- **Provide your modifications to LGPL components**: if you changed an LGPL component itself, you normally have to publish the source of those modifications (at least to the recipient).
- **Be careful about mobile distribution**: on iOS and in some store distribution forms "the user replaces the library" is hard to satisfy in practice. That is not "cannot use it", but it needs to be designed for up front in architecture and delivery (for example: prefer the system media frameworks, or use commercially licensed / alternative libraries).

## 4. Recommended boundary design (keep the core Apache-2.0)

If your goal is "core code under Apache-2.0, dependencies that drag in as little GPL as possible", the common pattern is:

- **A core playback abstraction layer under Apache-2.0**: define the `core` API (open media, tracks, clock, rendered frames, subtitles, buffering strategy).
- **A layered backend adapter**:
  - `backend_ffmpeg` (LGPL build, dynamically linked)
  - `backend_gstreamer` (choose license-compatible plugins)
  - `backend_avfoundation` (iOS/macOS system frameworks)
  - `backend_media3` (Android system / official libraries)
- **Plugins / optional dependencies must not ship GPL with the package**: even if something is only "loadable at runtime", distributing a GPL plugin together with your installer still triggers distribution obligations.

## 5. Documents and lists worth keeping in the repository

Keep at least:

- `THIRD_PARTY_NOTICES.md`: third-party dependency statements (see the repository template)
- `docs/licensing.md` (this document): selection principles and boundaries
- `LICENSES/`: collected license texts per dependency (optional but strongly recommended)
