#!/usr/bin/env bash
# Stage a macOS exe together with its complete runtime dylib closure.
#
# The macOS twin of the Windows DLL bug (2026-10 nightly): the binary was
# linked against whatever the build machine happened to have — brew's
# ffmpeg / openssl / fontconfig under /opt/homebrew — so the zip's lone
# `soar` could only start on a machine with the same brew packages (the
# vcpkg-managed deps are static via the apple triplets, but macOS FFmpeg
# arrives through pkg-config from brew, and libssl/libcrypto came from
# brew until vcpkg.json declared openssl).
#
# The walk is `otool -L` over everything already staged:
#   - /usr/lib and /System references are inbox — skipped, never copied
#   - @executable_path refs need no file
#   - @rpath / @loader_path refs name a companion dylib relative to the
#     referencing binary; they are resolved against that binary's ORIGINAL
#     directory (brew and vcpkg keep a formula's dylibs side by side), and
#     a companion that resolves nowhere is left alone
#   - anything else (absolute brew paths today) is copied into DEST/lib,
#     every referencing binary gets install_name_tool -change'd to
#     @loader_path[...] so the bundle is self-contained, and the bundled
#     dylib gets its id rewritten the same way
# A reference that resolves nowhere on disk fails the script — a silently
# incomplete package has to die here, not on a user's desk.
#
# usage: stage-macos-dylibs.sh EXE DEST
# env:   OTOOL / INSTALL_NAME_TOOL / CODESIGN  (defaults from PATH)
set -euo pipefail

if [ $# -lt 2 ]; then
  echo "usage: $0 EXE DEST" >&2
  exit 2
fi

exe=$1
dest=$2
otool_cmd=${OTOOL:-otool}
intool=${INSTALL_NAME_TOOL:-install_name_tool}
codesign_cmd=${CODESIGN:-codesign}

[ -f "$exe" ] || { echo "exe not found: $exe" >&2; exit 1; }

# otool -L prints the file itself on the first line; every dependency
# follows as "<path> (compatibility version ...)" — keep the path.
refs_of() {
  "$otool_cmd" -L "$1" | tail -n +2 | sed -n 's/^[[:space:]]*\([^[:space:]]*\).*/\1/p'
}

mkdir -p "$dest/lib"
base=$(basename "$exe")
cp "$exe" "$dest/$base"

# The macOS runners still ship Apple's bash 3.2, so no associative
# arrays: the seen-set is a newline-separated name list, and each queued
# item carries its ORIGINAL path next to the staged one — the original
# directory is where an @rpath / @loader_path companion can still be found.
seen_names=""
touched=("$dest/$base")
queue=("$dest/$base")
queue_src=("$exe")
i=0
while [ "$i" -lt "${#queue[@]}" ]; do
  current=${queue[$i]}
  current_src=${queue_src[$i]}
  i=$((i + 1))
  case "$current" in
    "$dest/lib/"*) rewrite="@loader_path" ;;
    *)             rewrite="@loader_path/lib" ;;
  esac
  while read -r ref; do
    case "$ref" in
      ""|/usr/lib/*|/System/*|@executable_path/*) continue ;;
    esac
    # An @rpath / @loader_path companion is not "no file": once the
    # referencing binary moves into the bundle, the rpath baked into it
    # points back at the machine that built it, and the bundle dies on
    # launch (2026-10-16 macOS red: freetype's shared brotli staged fine
    # while its @rpath/libbrotlicommon sibling was skipped). Resolve the
    # name against the referencing binary's original directory; one that
    # does not resolve there keeps the old skip — that ref was satisfied
    # some other way or never will be either way.
    src="$ref"
    case "$ref" in
      @rpath/*|@loader_path/*)
        candidate="$(dirname "$current_src")/$(basename "$ref")"
        if [ ! -f "$candidate" ]; then
          continue
        fi
        src="$candidate"
        ;;
    esac
    name=$(basename "$ref")
    bundled="$dest/lib/$name"
    if ! printf '%s\n' "$seen_names" | grep -qxF "$name"; then
      seen_names="$seen_names$name"$'\n'
      if [ ! -f "$src" ]; then
        echo "unresolvable dylib reference: $ref (needed by $current)" >&2
        exit 1
      fi
      cp "$src" "$bundled"
      # The bundled copy's install id is always @loader_path/<name>: the
      # file lives in lib/, next to the other bundled dylibs. Consumers
      # point at it via -change below.
      "$intool" -id "@loader_path/$name" "$bundled" 2>/dev/null || true
      touched+=("$bundled")
      queue+=("$bundled")
      queue_src+=("$src")
    fi
    # Repoint this referencing binary (exe → lib/, dylib → same dir).
    "$intool" -change "$ref" "$rewrite/$name" "$current"
  done < <(refs_of "$current" || true)
done

# install_name_tool invalidates the (ad-hoc) code signature, and an
# invalid signature gets the process killed outright on Apple Silicon —
# re-sign everything the tools touched.
for f in "${touched[@]}"; do
  "$codesign_cmd" --force --sign - "$f" >/dev/null 2>&1 || true
done

dll_count=$((${#queue[@]} - 1))
echo "staged $dll_count dylib(s) under $dest/lib next to $base:"
ls -1 "$dest" "$dest/lib"
