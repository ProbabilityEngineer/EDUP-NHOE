#!/bin/bash
# Build and run the four-actuator controller pulse test (macOS).
# Quit Elite first: the companion app would otherwise also be driving the
# controller and muddy the result. Feel for two pulses per actuator.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/mac-pulse-test"
if pgrep -f "EliteDangerous64.exe|ed-macos-haptics" >/dev/null; then
  echo "Elite or the companion app is running. Quit Elite, then retry." >&2; exit 1
fi
mkdir -p "$OUT"
if [ ! -x "$OUT/pulse" ] || [ "$ROOT/tools/mac_pulse_test.mm" -nt "$OUT/pulse" ]; then
  clang++ -fobjc-arc -std=c++17 "$ROOT/tools/mac_pulse_test.mm" -o "$OUT/pulse" \
    -framework Foundation -framework GameController -framework CoreHaptics -framework AppKit
fi
exec "$OUT/pulse"
