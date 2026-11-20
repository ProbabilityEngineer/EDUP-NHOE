#!/bin/bash
# Build the CrossOver dinput8 proxy (x64, MinGW GCC) and keep every build.
#
# Each build is copied to build/dinput8-builds/<YYYYMMDD-HHMMSS>-<branch>-<describe>/
# with build.json (describe, branch, built, sha256). The describe is the one
# compiled into the DLL (restoration-status.json "build"), read back from the
# generated header, so the folder name and the binary can never disagree.
# A -dirty describe means uncommitted changes: fine to test, not to send.
#
# Usage: tools/build_crossover.sh [--install [BOTTLE]]
#   --install copies the new DLL into the bottle's Elite folder (default
#   Elite-Arm); the DLL it replaces is already archived from its own build.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build/x64"
ARCHIVE="$ROOT/build/dinput8-builds"
INSTALL=""
if [ "${1:-}" = "--install" ]; then INSTALL="${2:-Elite-Arm}"; fi

# Homebrew's MinGW GCC must win over llvm-mingw's x86_64-w64-mingw32-gcc wrapper.
export PATH="/opt/homebrew/bin:$PATH"
command -v x86_64-w64-mingw32-gcc >/dev/null || { echo "x86_64-w64-mingw32-gcc not found" >&2; exit 1; }

if [ ! -f "$BUILD/build.ninja" ]; then
  cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/mingw64.cmake" -DBUILD_TESTING=OFF
fi
cmake --build "$BUILD" --target dinput_crossover_proxy

DLL="$BUILD/crossover-dinput8/dinput8.dll"
DESCRIBE=$(sed -n 's/^#define EDUP_BUILD_DESCRIBE "\(.*\)"$/\1/p' "$BUILD/generated/edup_version.h")
DESCRIBE=${DESCRIBE:-unknown}
BRANCH=$(git -C "$ROOT" rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)
BUILT=$(date +%Y%m%d-%H%M%S)
SHA=$(shasum -a 256 "$DLL" | cut -d' ' -f1)

# Same bytes as the newest archived build: don't archive a duplicate.
LAST=$(ls -1d "$ARCHIVE"/*/ 2>/dev/null | tail -1 || true)
if [ -n "$LAST" ] && [ -f "$LAST/dinput8.dll" ] && [ "$(shasum -a 256 "$LAST/dinput8.dll" | cut -d' ' -f1)" = "$SHA" ]; then
  DEST="${LAST%/}"
  echo "unchanged since $(basename "$DEST")"
else
  DEST="$ARCHIVE/$BUILT-${BRANCH//\//-}-$DESCRIBE"
  mkdir -p "$DEST"
  cp "$DLL" "$DEST/dinput8.dll"
  printf '{\n "describe": "%s",\n "branch": "%s",\n "built": "%s",\n "target": "crossover-dinput8 x64",\n "sha256": "%s"\n}\n' \
    "$DESCRIBE" "$BRANCH" "$BUILT" "$SHA" > "$DEST/build.json"
fi
echo "build    $DESCRIBE"
echo "sha256   $SHA"
echo "archived $DEST"
case "$DESCRIBE" in *-dirty) echo "WARNING: built from uncommitted changes (-dirty). Commit before sending this DLL to anyone." >&2 ;; esac

if [ -n "$INSTALL" ]; then
  BOTTLE_PATH="$(cd "$HOME/Library/Application Support/CrossOver/Bottles/$INSTALL" && pwd -P)"
  GAME="$BOTTLE_PATH/drive_c/Program Files (x86)/Frontier/EDLaunch/Products/elite-dangerous-odyssey-64"
  [ -d "$GAME" ] || { echo "game folder not found in bottle $INSTALL" >&2; exit 1; }
  cp "$DEST/dinput8.dll" "$GAME/dinput8.dll"
  echo "installed into $INSTALL"
fi
