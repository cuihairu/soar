[English](/build) | [中文](/zh/build)

# Build (C++17 + CMake + vcpkg)

## Prerequisites

- CMake 3.20+
- A C++17 compiler (Clang/GCC/MSVC)
- Ninja (recommended)
- vcpkg (manifest mode recommended)

## 1) Configure vcpkg

Set the `VCPKG_ROOT` environment variable to point at your vcpkg checkout, for example:

- macOS/Linux: `export VCPKG_ROOT=/path/to/vcpkg`
- Windows (PowerShell): `$env:VCPKG_ROOT="C:\path\to\vcpkg"`

## 2) Build

Use CMake Presets:

- Debug (vcpkg): `cmake --preset default && cmake --build --preset default`
- Release (vcpkg): `cmake --preset release && cmake --build --preset release`
- Linux system packages (no vcpkg; installs the FFmpeg development headers plus libsdl2-dev/libfmt-dev/libboost-dev via apt): `cmake --preset linux-system && cmake --build --preset linux-system`, then test with `ctest --preset linux-system`

libtorrent (the P2P download engine, docs/mvp.md §5 P4) is vendored in `third_party/libtorrent/` (pinned at v2.0.15, BSD-3, see that directory's `VENDOR.md`) and is compiled as part of the build; it only needs the Boost headers (apt `libboost-dev`, vcpkg `boost-system`). BitTorrent protocol encryption (MSE) is enabled by default (`SOAR_ENABLE_TORRENT_ENCRYPTION`); the handshake uses the DH/RC4/SHA-1 implementation bundled with the library, see docs/mvp.md §5 P4b-5. The vcpkg manifest declares `openssl`: libtorrent's own `find_package(OpenSSL)` has to resolve to vcpkg's copy rather than whatever the build machine happens to have. On Windows the build links vcpkg's DLLs (they ship inside the release bundle's closure — see BUGS.md #1 for the root cause); on macOS it links vcpkg's static triplets.

Output directory: `build/` (`build-system/` for linux-system).

## 3) Run

- Windowed playback: `./build/soar <path-or-url>` (FFmpeg is the default backend when it is compiled in)
- Start with no source, into the empty main screen: `./build/soar --gui` (`O` key / click the poster / drag and drop to open a file; the GUI front end `soarw.exe` does the same thing on double-click — no source does not print the usage text, BUGS.md #3)
- CLI smoke test (no window): `./build/soar --headless --backend=ffmpeg <path-or-url>`
- Core demo (no multimedia dependencies): `./build/soar --backend=null <path-or-url>`

Option reference:

- `--headless`: no window; runs open → play → seek → pause → stop and exits. Used for smoke tests.
- `--version`: prints the version and exits.
- `--help` / `-h`: prints the usage and exits (0).
- `--gui`: starts with no source into the empty main screen (the GUI front end `soarw.exe` does this implicitly); a terminal build (the console `soar.exe`, or headless) with no source still prints the usage and exits 2.
- `--backend=`: selects the backend (`ffmpeg`, `null`); default `ffmpeg` (when FFmpeg is compiled in, otherwise `null`); if the requested backend is unavailable the app falls back and says so.
- `--cache-dir=`: persists `http://` downloads on disk so the same URL can be replayed offline, resumed after an interruption, and filled in through Range gaps (https goes straight through without caching, docs/mvp.md §5 P3).
- A `.torrent` positional argument or a `magnet:` URI is a P2P source (docs/mvp.md §5 P4): a local libtorrent session downloads sequentially and the result is bridged as `http://127.0.0.1:<port>/` into the normal playback pipeline. `--torrent-store=` (where data is written to disk, default `<tmp>/soar-torrent`), `--torrent-peer=host:port` (peer directly, repeatable), `--torrent-index=N` (which file to play in a multi-file torrent), `--torrent-list` (prints the torrent's file table and exits; for a magnet the swarm is queried for the metadata first — pair with `--torrent-index` to look at the table before choosing). A magnet takes `xt=urn:btih:` (either 40 hex digits or 32 base32 characters); trackers come from `tr=` inside the magnet, peers from an embedded `x.pe=` or a direct `--torrent-peer=`, and public DHT bootstrapping happens automatically. If the swarm delivers no metadata within 60 seconds the app reports the failure explicitly and exits. When playing a multi-file torrent, the window title and poster show the chosen file's name rather than the `127.0.0.1:<port>` bridge address. In windowed mode P2P sources use the asynchronous open path (P4b-4): the window opens at startup and the poster shows `connecting to swarm - N peers` until the metadata arrives and playback starts automatically (a `Downloading N%` chip is visible in the same period), and the title bar switches to `soar - <filename>`. If no metadata arrives within 60 seconds a toast reports the error and the window stays; `--headless` and `--torrent-list` keep their synchronous blocking contract (print the table or the error, then exit).
- Local P2P walkthrough tool: `./build/seed_torrent --file=<media-or-directory> --out=<x.torrent> [--port=6881] [--rate-kb=N]` (a directory makes a multi-file torrent whose root is the directory name; directory entries are sorted by name into the file table, so `--torrent-index=N` points at the same file on every machine — libtorrent's add_files follows the platform's readdir order, which differs between file systems; the rate limit simulates a slow peer). Then play it with `soar --torrent-peer=127.0.0.1:6881 <x.torrent>`, or with the equivalent magnet: `soar 'magnet:?xt=urn:btih:<info hash printed by the seeder>&x.pe=127.0.0.1:6881'`

## 4) Test

```bash
ctest --preset default
```

Unit tests use `NullBackend` as the test double and need no FFmpeg or SDL2; set `SOAR_BUILD_TESTS=OFF` to disable building the tests.

## 5) Optional capability switches

CMake options (all default to `ON`; a missing dependency degrades the build rather than failing it):

| Option | Effect | Without it |
|---|---|---|
| `SOAR_ENABLE_FFMPEG` | FFmpeg backend (probed via pkg-config for libav*/sw*) | `NullBackend` only; the CLI still works |
| `SOAR_ENABLE_SDL2` | SDL2 window and audio (needs **SDL ≥ 2.0.18**) | CLI only (`--headless`) |
| `SOAR_ENABLE_IMGUI` | The player UI overlay drawn on the window (Dear ImGui v1.91.9b, `imgui_impl_sdlrenderer2`) | Falls back to a bare video window (no control bar) |
| `SOAR_ENABLE_LIBASS` | libass subtitle rendering (Linux pkg-config `libass-dev`; mac/win via the vcpkg `libass` port) | ASS/SSA scripts degrade to plain-text subtitles |
| `SOAR_ENABLE_TORRENT_ENCRYPTION` | BitTorrent protocol encryption MSE (docs/mvp.md §5 P4b-5) | Cannot connect to peers that require encryption |
| `SOAR_BUILD_APP` / `SOAR_BUILD_TESTS` | App / test targets | — |

`SOAR_ENABLE_IMGUI` needs GitHub to be reachable at **configure time**: CMake clones ImGui into `<build>/_deps/imgui-src` with `--depth 1` (it never enters the repository or a release bundle). With no network or no git the app prints a notice and degrades to the bare window; `-DSOAR_ENABLE_IMGUI=OFF` skips the fetch entirely. A directory that is already cloned is reused by later configures.
