# Third-Party Notices

This project is licensed under the Apache License, Version 2.0.  
Distributions that bundle third-party components must comply with the licenses of those components.

## How to use this file

- List every third-party library you **redistribute** (source or binary).
- Include: name, homepage/source, license, copyright.
- If you modified the component, note it and provide the source as required by its license.

## Bundled components

- libtorrent (rasterbar)
  - Source: https://github.com/arvidn/libtorrent (tag `v2.0.15`,
    commit `1eb18faeae156d8dbbab42935c082f8b81f50989`)
  - License: BSD 3-Clause (the full text travels with the vendored tree at
    `third_party/libtorrent/COPYING` and must be included in any
    distribution of the binary)
  - Copyright: Copyright (c) 2003-2020, Arvid Norberg
  - Notes: Vendored (pruned tree, one guarded `add_subdirectory`; see
    `third_party/libtorrent/VENDOR.md`) and statically linked. It is the
    BitTorrent download kernel behind the P2P http bridge in `src/p2p/`
    (docs/mvp.md §5 P4); no upstream executable is built or shipped.

### Build-time fetched (not vendored in this repository)

These are compiled into the `soar` binary by the build, so a binary
distribution must carry their license texts alongside it. The sources
themselves are **not** committed here (Dear ImGui is cloned into the build
tree at configure time; see docs/build.md).

- Dear ImGui
  - Source: https://github.com/ocornut/imgui (tag `v1.91.9b`)
  - License: MIT
  - Copyright: Copyright (c) 2014-2025 Omar Cornut
  - Notes: Player UI overlay (`SOAR_ENABLE_IMGUI`, default ON), built from
    `imgui*.cpp` plus the `imgui_impl_sdl2` and `imgui_impl_sdlrenderer2`
    backends. Vendored by the build; the upstream `LICENSE.txt` travels
    with the cloned tree and must be included in releases.

Runtime libraries bundled in the release packages (this used to read
"resolved from the system, not redistributed by us" — that is no longer
true): the nightly zips / deb / rpm / setup.exe carry the FFmpeg, SDL2,
libass and OpenSSL runtime libraries (plus the MSVC runtime on Windows),
staged by `scripts/stage-windows-dlls.sh` (dumpbin dependency closure),
`scripts/stage-macos-dylibs.sh` (`otool -L` closure + install_name_tool
repoint) and the Linux packager. Per-platform layouts are documented in
each package's `PLATFORM-NOTES.txt`. License highlights for the bundled
set — the license texts must travel with any distribution:

- SDL2 — zlib license
- FFmpeg (libavformat/libavcodec/swresample/swscale) — LGPL-2.1-or-later
  build only; we do not distribute GPL builds or GPL-enabled components
- libass — ISC
- OpenSSL (libssl/libcrypto) — Apache-2.0
- fmt — MIT
- MSVC runtime (MSVCP140/VCRUNTIME140/VCRUNTIME140_1, Windows) —
  redistributable under Microsoft's terms

(Bundling form varies by platform — shared libraries under `lib/` on
Linux/macOS, DLLs next to the exe on Windows, static where the vcpkg
apple triplets link statically; see each package's PLATFORM-NOTES.txt.)

Build-time only (linked in, never shipped as separate files): doctest
(MIT), the vendored libtorrent listed above, and the Dear ImGui clone
described above.

