# Vendored: libtorrent (rasterbar)

- Upstream: https://github.com/arvidn/libtorrent
- Pinned tag: `v2.0.15`
- Upstream commit: `1eb18faeae156d8dbbab42935c082f8b81f50989`
- License: BSD 3-Clause (see `COPYING`)
- Vendored: 2026-10-01, for docs/mvp.md §5 P4 (P2P streaming).

## Why vendored

libtorrent is the BitTorrent download kernel behind `src/p2p/` (TorrentStream,
docs/mvp.md §5 P4). It is used as a library only: no upstream executable
(tools/examples) is built or shipped. Soar's own code is everything under
`src/p2p/` and `tools/`.

## Differences from the upstream tree (pruned to what the build consumes)

- Pruned directories: `docs/`, `examples/`, `simulation/`, `test/`,
  `tools/`, `bindings/`, `deps/asio-gnutls/` (only used with GnuTLS builds)
  and the b2 build files (`Jamfile`, `Jamroot.jam`, `project-config.jam`,
  `Makefile`, `setup.py`).
- One-line CMake patch, marked `VENDOR PATCH (soar)` in `CMakeLists.txt`:
  `add_subdirectory(bindings)` is guarded by the `python-bindings` option
  (upstream adds `bindings/` unconditionally; that directory is pruned here).

## How it is built here

Added with `add_subdirectory(third_party/libtorrent)` from the repository root.
The superproject pins the options before the subdirectory:

- `BUILD_SHARED_LIBS=OFF` (static link into the `soar` binary)
- `encryption=ON` (default, via the superproject's
  `SOAR_ENABLE_TORRENT_ENCRYPTION` option; docs/mvp.md §5 P4b-5) — MSE
  stream encryption. The P4a batch originally pinned `encryption=OFF`
  (bundled SHA-1/SHA-256 only, plain peers reachable); that boundary was
  closed by P4b-5: the 2.0.15 handshake is self-contained (DH over
  boost::multiprecision plus the bundled RC4/SHA-1), so enabling it adds
  no external dependency. Peers that require encryption are now reachable.
- OpenSSL: the superproject's `vcpkg.json` declares `openssl` so
  libtorrent's `find_package(OpenSSL)` resolves to vcpkg instead of
  whatever the build host has (Windows links vcpkg's DLLs, staged into
  the package by the dependency closure; macOS links vcpkg's static
  apple triplet). That lookup runs regardless of the encryption flag.
- `dht=ON`, `streaming=ON` (piece deadlines — required for stream-on-demand),
  `logging=ON`, everything else at upstream defaults (tests/examples/tools/
  python-bindings all OFF).
- Only external requirement: Boost headers (`Boost::headers`, header-only).
  With apt (`libboost-dev`) that is one monolithic package; with vcpkg the
  boost ports are modular, so `vcpkg.json` declares the ports matching the
  `#include <boost/...>` paths this vendored tree actually uses (asio,
  container-hash, crc, date-time, intrusive, logic, multi-index,
  multiprecision, optional, pool, range, smart-ptr, system, utility,
  variant) — their own transitive dependencies come along automatically.

To update the pin: download the new release tarball from upstream, re-apply
the pruning and the one-line patch above, update this file (tag + commit +
date), and record the bump in the batch notes.
