#!/bin/bash
# Guided ship-weapon audio-event capture for EDUP-NHOE on a CrossOver bottle.
#
# Run this while Elite is running with the crossover dinput8 proxy, you are
# in space and safe to fire. It creates the capture marker, waits until the
# mod has claimed it (recording started), then speaks a cue for each weapon:
# "<weapon>, get ready" (switch fire group), "fire", "stop". Every cue is
# logged with a timestamp so recorded audio events can be matched to weapons.
# At the end the marker is removed, which makes the mod close the recording,
# and the session's files are copied into the capture folder
# (~/Library/Logs/edup-nhoe-capture/<time>/session/), because the mod keeps
# only the newest few session folders and deletes older ones.
#
# Usage: tools/weapon_capture.sh [--bottle NAME] [--fire SEC] [--ready SEC]
#                                [weapon ...]
# Default weapons: pulse laser, burst laser, rail gun, cannon, multi-cannon,
# fragment cannon, missile rack. The mod caps a capture at 10 minutes.
set -u

BOTTLE="Elite-Arm"
FIRE=8
READY=8
QUIET=6
while [ $# -gt 0 ]; do
  case "$1" in
    --bottle) BOTTLE="$2"; shift 2 ;;
    --fire) FIRE="$2"; shift 2 ;;
    --ready) READY="$2"; shift 2 ;;
    -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
    --) shift; break ;;
    -*) echo "unknown option: $1" >&2; exit 2 ;;
    *) break ;;
  esac
done
if [ $# -gt 0 ]; then WEAPONS=("$@"); else
  WEAPONS=("pulse laser" "burst laser" "rail gun" "cannon" "multi-cannon" "fragment cannon" "missile rack")
fi

BOTTLE_PATH="$(cd "$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE" 2>/dev/null && pwd -P)"
[ -n "$BOTTLE_PATH" ] || { echo "bottle not found: $BOTTLE" >&2; exit 1; }
GAME="$BOTTLE_PATH/drive_c/Program Files (x86)/Frontier/EDLaunch/Products/elite-dangerous-odyssey-64"
MARKER="$GAME/ed-onfoot-trace.enable"
SESSIONS="$BOTTLE_PATH/drive_c/users/crossover/AppData/Local/Temp/EDRumbleRestoration"

pgrep -f "EliteDangerous64.exe" >/dev/null || { echo "Elite is not running. Launch it, get into space, then run this again." >&2; exit 1; }
if [ -e "$MARKER" ]; then
  echo "A capture marker already exists: $MARKER" >&2
  echo "It may be left from a crashed capture. Delete it if no capture is running, then retry." >&2
  exit 1
fi

OUT="$HOME/Library/Logs/edup-nhoe-capture/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT"
TIMELINE="$OUT/timeline.tsv"
now_ms() { python3 -c 'import time;print(int(time.time()*1000))'; }
mark() { printf '%s\t%s\t%s\n' "$(now_ms)" "$(date '+%H:%M:%S')" "$1" | tee -a "$TIMELINE"; }
speak() { say -r 210 "$1" & }

SESSION=""
cleanup() {
  rm -f "$MARKER"
  mark "marker removed"
}
# Give the mod time to close the recording, then keep a copy of the session.
copy_session() {
  [ -n "$SESSION" ] && [ -d "$SESSIONS/$SESSION" ] || return 0
  sleep 3
  mkdir -p "$OUT/session"
  cp -p "$SESSIONS/$SESSION"/* "$OUT/session/" && echo "Session copied to $OUT/session"
}
trap 'cleanup; copy_session; echo "Interrupted. Capture folder: $OUT"; exit 130' INT TERM

printf 'epoch_ms\ttime\tevent\n' > "$TIMELINE"
printf 'ed-onfoot-event-trace-v1\n' > "$MARKER"
mark "marker created"

# The mod polls about every half second; a claimed marker starts with '!'.
claimed=0
for _ in $(seq 1 60); do
  if [ "$(head -c 1 "$MARKER" 2>/dev/null)" = "!" ]; then claimed=1; break; fi
  sleep 0.5
done
if [ "$claimed" != 1 ]; then
  rm -f "$MARKER"
  echo "The mod did not start recording within 30 s (is the crossover dinput8 proxy loaded?)." >&2
  exit 1
fi
SESSION=$(ls -t "$SESSIONS" 2>/dev/null | head -1)
mark "recording started session=$SESSION"
echo "session=$SESSION" > "$OUT/info.txt"

speak "Recording. Hold fire."; mark "baseline quiet start"; sleep 8; mark "baseline quiet end"

for weapon in "${WEAPONS[@]}"; do
  speak "$weapon. Get ready."; mark "ready $weapon"; sleep "$READY"
  speak "Fire."; mark "fire $weapon"; sleep "$FIRE"
  speak "Stop."; mark "stop $weapon"; sleep "$QUIET"
done

speak "Done. Thank you."
cleanup
copy_session
echo
echo "Capture folder: $OUT"
ls -la "$OUT" "$OUT/session" 2>/dev/null
echo "Report: tools/weapon_capture_report.py \"$OUT\""
