#!/usr/bin/env bash
# Stage a Windows exe together with its complete runtime DLL closure.
#
# The 2026-10 nightly bug this exists for: the package step used to ship
# whatever `cp build/*.dll` happened to find — vcpkg's applocal deployment
# had put SDL2.dll and fmt.dll next to the exe but not the openssl pair
# the vendored libtorrent links (libssl-3-x64.dll / libcrypto-3-x64.dll),
# and nothing bundled the MSVC runtime at all, so a user's machine one
# VC-redistributable away from a clean install got "缺少 DLL" and no exe.
#
# The walk is dumpbin /dependents over everything already staged, resolved
# in a fixed order:
#   1. the destination dir   — already-staged DLLs' own imports
#   2. vcpkg release bin     — SDL2 / fmt / openssl / (libass, FFmpeg once
#                              the Windows build grows them) ...
#   3. vcpkg debug bin       — Debug builds import debug-suffixed names
#   4. VC Redist CRT folder  — MSVCP140 / VCRUNTIME140 / ... (always
#                              bundled, even though this runner has them:
#                              System32 is exactly what would mask a gap)
#   4b. Redist debug_nonredist + Windows SDK ucrt dirs — the debug CRT
#       twins (MSVCP140D / VCRUNTIME140D / ucrtbased) that a Debug build
#       imports. Only ci.yml stages a Debug exe; the nightly Release
#       walk never asks for these names.
#   5. System32             — inbox OS DLLs, skipped, never copied
#                              (api-ms-win-* UCRT included: Win10+ inbox)
# A name that resolves nowhere fails the script with the full list — a
# silently incomplete package has to die here, not on a user's desk.
#
# usage: stage-windows-dlls.sh EXE DEST VC_BIN [VC_DEBUG_BIN]
# env:   DUMPBIN         dumpbin binary        (default: dumpbin on PATH)
#        VCINSTALLDIR    VC root, for Redist   (msvc-dev-cmd sets it)
#        VCToolsInstallDir  tools root, CRT fallback
#        WindowsSdkDir / WindowsSDKVersion  SDK root, for ucrtbased.dll
#        SYSTEM32_DIR    system dir            (default: $WINDIR/System32)
#
# dumpbin is invoked with dash-form options (-dependents, not /dependents):
# under Git Bash the MSYS layer rewrites a leading "/dependents" into a
# Windows path before dumpbin ever sees it, and dumpbin dies with a
# swallowed diagnostics-only exit (run 37127854221). The LINK-family
# option parser accepts both prefixes, so dash form is portable.
set -euo pipefail

if [ $# -lt 3 ]; then
  echo "usage: $0 EXE DEST VC_BIN [VC_DEBUG_BIN]" >&2
  exit 2
fi

exe=$1
dest=$2
vcpkg_bin=$3
vcpkg_debug_bin=${4:-}
dumpbin_cmd=${DUMPBIN:-dumpbin}

# Windows-style env arrives backslash-separated; normalize so globs and
# tests below stay plain POSIX paths under Git Bash.
vcinstalldir=${VCINSTALLDIR:-}
vcinstalldir=${vcinstalldir//\\//}
vctools=${VCToolsInstallDir:-}
vctools=${vctools//\\//}
sys32=${SYSTEM32_DIR:-${WINDIR:-C:/Windows}/System32}
sys32=${sys32//\\//}

[ -f "$exe" ] || { echo "exe not found: $exe" >&2; exit 1; }
# A wrong vcpkg path would make every non-system import "missing" anyway,
# but failing up front names the actual mistake.
[ -d "$vcpkg_bin" ] || { echo "vcpkg bin dir not found: $vcpkg_bin" >&2; exit 1; }

# Newest VC redistributable CRT folder (…/VC/Redist/MSVC/<ver>/x64/
# Microsoft.VC14x.CRT). Empty when the env is absent — then the CRT falls
# back to the tools dir and finally System32 below.
vc_redist=""
if [ -n "$vcinstalldir" ]; then
  vc_redist=$(ls -d "$vcinstalldir"/Redist/MSVC/*/x64/Microsoft.VC14*.CRT 2>/dev/null | sort -V | tail -1 || true)
fi
# …and its debug twin (…/debug_nonredist/x64/Microsoft.VC14x.DebugCRT),
# the only place MSVCP140D / VCRUNTIME140D live. Never redistributed:
# the nightly package is a Release build and never imports them; only
# ci.yml's Debug stage-verify walk resolves from here.
vc_redist_debug=""
if [ -n "$vcinstalldir" ]; then
  vc_redist_debug=$(ls -d "$vcinstalldir"/Redist/MSVC/*/debug_nonredist/x64/Microsoft.VC14*.DebugCRT 2>/dev/null | sort -V | tail -1 || true)
fi
vc_tools=""
if [ -n "$vctools" ]; then
  arch=${VSCMD_ARG_TGT_ARCH:-x64}
  vc_tools="$vctools/bin/Host$arch/$arch"
fi
# Prefer PATH (msvc-dev-cmd puts the tools bin there); fall back to the
# tools dir so a bash step without the MSVC PATH still finds dumpbin.
if [ "$dumpbin_cmd" = dumpbin ] && ! command -v dumpbin >/dev/null 2>&1 \
   && [ -n "$vc_tools" ] && [ -f "$vc_tools/dumpbin.exe" ]; then
  dumpbin_cmd="$vc_tools/dumpbin.exe"
fi

# The CRT allow-list: names permitted to fall through to the tools dir or
# even System32. Everything else may only come from vcpkg — if it is not
# there it is genuinely missing, not an OS inbox file.
is_crt() {
  case "$1" in
    msvcp140.dll|msvcp140_1.dll|msvcp140_2.dll|msvcp140_3.dll| \
    vcruntime140.dll|vcruntime140_1.dll|concrt140.dll|msuvcp140.dll) return 0 ;;
    *) return 1 ;;
  esac
}

# Case-insensitive lookup: import-table names and on-disk names disagree
# on case (MSVCP140.dll vs msvcp140.dll), which Windows' filesystem hides
# — and which the local harness on a case-sensitive one does not.
find_in() {
  if [ -z "$1" ] || [ ! -d "$1" ]; then
    return 0
  fi
  find "$1" -maxdepth 1 -iname "$2" -print -quit
}

# ucrtbased.dll — the debug UCRT a /MDd build imports — ships with the
# Windows SDK, not VC (neither Redist nor System32 ever has it).
sdk_ucrt=""
sdk=${WindowsSdkDir:-}
sdk=${sdk//\\//}
sdkver=${WindowsSDKVersion:-}
if [ -n "$sdk" ] && [ -n "$sdkver" ]; then
  for d in "$sdk/bin/$sdkver/x64/ucrt" "$sdk/Redist/$sdkver/ucrt/DLLs/x64"; do
    if [ -n "$(find_in "$d" ucrtbased.dll)" ]; then
      sdk_ucrt=$d
      break
    fi
  done
fi

mkdir -p "$dest"
base=$(basename "$exe")
cp "$exe" "$dest/$base"

declare -A seen=()
missing=()
queue=("$dest/$base")
i=0
while [ "$i" -lt "${#queue[@]}" ]; do
  current=${queue[$i]}
  i=$((i + 1))

  # dumpbin prints one indented dependency per line under "Image has the
  # following dependencies:" (and the delay-load section, if any). The
  # anchored sed keeps only bare "<name>.dll" lines, so the header, the
  # file path and the Summary block never leak in. Dash-form options: a
  # "/dependents" would be rewritten into a path by Git Bash's MSYS layer
  # (see the header note) — and keep dumpbin's own stderr flowing to the
  # log so a failure here names its reason instead of exiting silently.
  deps=$("$dumpbin_cmd" -nologo -dependents "$current") && drc=0 || drc=$?
  if [ "$drc" -ne 0 ]; then
    echo "dumpbin -dependents failed (exit $drc) on: $current" >&2
    exit 1
  fi
  while read -r dll; do
    if [ -z "$dll" ]; then
      continue
    fi
    key=$(printf '%s' "$dll" | tr '[:upper:]' '[:lower:]')
    if [ -n "${seen[$key]:-}" ]; then
      continue
    fi
    seen[$key]=1

    src=""
    for dir in "$dest" "$vcpkg_bin" "$vcpkg_debug_bin" "$vc_redist" \
               "$vc_redist_debug" "$sdk_ucrt"; do
      src=$(find_in "$dir" "$dll")
      if [ -n "$src" ]; then
        break
      fi
    done
    if [ -z "$src" ] && is_crt "$key"; then
      src=$(find_in "$vc_tools" "$dll")
      if [ -z "$src" ]; then
        src=$(find_in "$sys32" "$dll")
      fi
    fi

    if [ -n "$src" ]; then
      if [ -z "$(find_in "$dest" "$dll")" ]; then
        cp "$src" "$dest/$dll"
      fi
      queue+=("$dest/$dll")
    elif [ -z "$(find_in "$sys32" "$dll")" ]; then
      missing+=("$dll")
    fi
    # Found in System32 and not CRT-allow-listed: an inbox OS DLL
    # (kernel32, shell32, api-ms-win-crt-* …) — resolved by the OS on
    # every target machine, deliberately not bundled.
  done < <(printf '%s\n' "$deps" |
    sed -n 's/^[[:space:]]\{1,\}\([A-Za-z0-9_.-]\{1,\}\.dll\)[[:space:]]*$/\1/Ip')
done

if [ "${#missing[@]}" -gt 0 ]; then
  echo "unresolvable DLL imports (absent from vcpkg, VC redist and System32):" >&2
  printf '  %s\n' "${missing[@]}" >&2
  exit 1
fi

dll_count=$((${#queue[@]} - 1))
echo "staged $dll_count DLL(s) next to $base:"
ls -1 "$dest"
