# Third-Party Notices

This project is licensed under the Apache License, Version 2.0.  
Distributions that bundle third-party components must comply with the licenses of those components.

## How to use this file

- List every third-party library you **redistribute** (source or binary).
- Include: name, homepage/source, license, copyright.
- If you modified the component, note it and provide the source as required by its license.

## Bundled components

> Fill in when you start bundling dependencies.

- Name:
  - Source:
  - License:
  - Copyright:
  - Notes:

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

Runtime/optional capabilities already listed in docs/licensing.md but
resolved from the system (not redistributed by us): SDL2 (zlib),
FFmpeg (LGPL-2.1-or-later builds only), fmt (MIT), doctest (MIT).

