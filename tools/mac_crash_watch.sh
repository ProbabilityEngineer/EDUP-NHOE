#!/bin/bash
# Watch a CrossOver bottle while testing a dinput8 build and log what would
# lead up to a macOS "zone map exhausted ... [VM map entries]" kernel panic.
#
# Every sample records the kernel's VM-map-entry count (zprint, no root
# needed) and memory use of the bottle's Wine processes. Each new or changed
# EDRumbleRestoration status file is copied as it appears. If the VM-map-entry
# count passes --kill-at, the script snapshots the processes and force-quits
# every process in the bottle, so the Mac does not panic. Normal is well under
# 1 million entries; the Nov 20 panic happened at about 180 million.
#
# Usage: tools/mac_crash_watch.sh [--bottle NAME] [--interval SEC]
#                                 [--warn-at N] [--kill-at N] [--no-kill]
# Stop with Ctrl-C. Logs: ~/Library/Logs/edup-nhoe-watch/<time>/
set -u

BOTTLE=""
BOTTLES_DIR="$HOME/Library/Application Support/CrossOver/Bottles"
INTERVAL=2
WARN_AT=2000000
KILL_AT=20000000
KILL=1

while [ $# -gt 0 ]; do
  case "$1" in
    --bottle) BOTTLE="$2"; shift 2 ;;
    --interval) INTERVAL="$2"; shift 2 ;;
    --warn-at) WARN_AT="$2"; shift 2 ;;
    --kill-at) KILL_AT="$2"; shift 2 ;;
    --no-kill) KILL=0; shift ;;
    -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

. "$(dirname "$0")/crossover_bottle.sh"
BOTTLE="$(edup_bottle "$BOTTLE")" || exit 1
BOTTLE_PATH="$(cd "$BOTTLES_DIR/$BOTTLE" 2>/dev/null && pwd -P)"
if [ -z "$BOTTLE_PATH" ]; then echo "bottle not found: $BOTTLES_DIR/$BOTTLE" >&2; exit 1; fi
STATUS_ROOT="$BOTTLE_PATH/drive_c/users/crossover/AppData/Local/Temp/EDRumbleRestoration"
GAME_DIR="$BOTTLE_PATH/drive_c/Program Files (x86)/Frontier/EDLaunch/Products/elite-dangerous-odyssey-64"

OUT="$HOME/Library/Logs/edup-nhoe-watch/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT/status"
LOG="$OUT/watch.log"
EVENTS="$OUT/events.log"

say() { echo "$(date '+%H:%M:%S') $*" | tee -a "$EVENTS"; }

vm_entries() {
  zprint "VM map entries" 2>/dev/null | awk '$1=="VM.map.entries"{print $7; exit}'
}

# PIDs belonging to this bottle. Wine rewrites its processes' argv/environment,
# so match on the working directory (inside the bottle for Windows processes
# and the wineserver) or, for launcher/helper processes, the command line or
# environment naming the bottle.
bottle_pids() {
  local cands pid cwd
  cands=$(pgrep -u "$(id -u)" -f 'wine|\.exe|ed-macos-haptics|GameInput|CrossOver' 2>/dev/null | tr '\n' ',' | sed 's/,$//')
  [ -n "$cands" ] || return 0
  {
  lsof -a -d cwd -Fpn -p "$cands" 2>/dev/null | awk -v b="$BOTTLE_PATH" '
    /^p/ {pid=substr($0,2)}
    /^n/ {if (index(substr($0,2), b) == 1) print pid}'
  for pid in ${cands//,/ }; do
    if ps -E -ww -o command= -p "$pid" 2>/dev/null | grep -qF -e "WINEPREFIX=$BOTTLE_PATH" -e "--bottle $BOTTLE " -e "CX_BOTTLE=$BOTTLE "; then
      echo "$pid"
    fi
  done
  } | sort -u
}

snapshot() {  # $1 = file
  {
    echo "== $(date '+%F %T') vm_map_entries=$(vm_entries)"
    echo "-- bottle processes (pid rss_kb vsz_kb command)"
    for p in $(bottle_pids); do ps -o pid=,rss=,vsz=,command= -p "$p" 2>/dev/null | cut -c1-200; done
    echo "-- top 15 processes by rss"
    ps -axo pid,rss,vsz,comm -r | head -16
  } >> "$1"
}

{
  echo "bottle=$BOTTLE path=$BOTTLE_PATH"
  echo "interval=${INTERVAL}s warn_at=$WARN_AT kill_at=$KILL_AT kill=$KILL"
  echo "started=$(date '+%F %T') macOS=$(sw_vers -productVersion) build=$(sw_vers -buildVersion)"
  [ -f "$GAME_DIR/dinput8.dll" ] && echo "dinput8.dll sha256=$(shasum -a 256 "$GAME_DIR/dinput8.dll" | cut -d' ' -f1) size=$(stat -f %z "$GAME_DIR/dinput8.dll")"
  [ -f "$GAME_DIR/ed-onfoot-haptics.ini" ] && { echo "--- ed-onfoot-haptics.ini"; cat "$GAME_DIR/ed-onfoot-haptics.ini"; }
} > "$OUT/info.txt"

printf 'time\tvm_map_entries\tdelta\tbottle_procs\tbottle_rss_mb\tbottle_vsz_gb\tbiggest\n' > "$LOG"
say "watching bottle '$BOTTLE'; logs in $OUT"
[ "$KILL" = 1 ] && say "safety: will force-quit the bottle's processes if VM map entries exceed $KILL_AT" \
                || say "safety kill disabled (--no-kill)"
trap 'say "stopped"; echo "Logs: $OUT"; exit 0' INT TERM

seen_status=""
prev=$(vm_entries); prev=${prev:-0}
warned=0
while :; do
  now=$(vm_entries); now=${now:-0}
  pids=$(bottle_pids)
  n=0; rss=0; vsz=0; big=""; bigrss=0
  for p in $pids; do
    line=$(ps -o rss=,vsz=,comm= -p "$p" 2>/dev/null) || continue
    set -- $line
    [ $# -ge 3 ] || continue
    n=$((n+1)); rss=$((rss+$1)); vsz=$((vsz+$2))
    if [ "$1" -gt "$bigrss" ]; then bigrss=$1; big="$p:$(basename "$3")"; fi
  done
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$(date '+%H:%M:%S')" "$now" "$((now-prev))" "$n" "$((rss/1024))" "$((vsz/1048576))" "$big" >> "$LOG"

  if [ "$now" -ge "$WARN_AT" ] && [ "$warned" = 0 ]; then
    warned=1; say "WARNING: VM map entries at $now (normal is well under 1M)"; snapshot "$OUT/warn-snapshot.txt"
  fi
  if [ "$now" -lt "$WARN_AT" ]; then warned=0; fi

  if [ "$KILL" = 1 ] && [ "$now" -ge "$KILL_AT" ]; then
    say "DANGER: VM map entries at $now >= $KILL_AT; force-quitting bottle processes: $(echo $pids)"
    snapshot "$OUT/kill-snapshot.txt"
    [ -n "$pids" ] && kill -KILL $pids 2>/dev/null
    sleep 3
    say "after kill: VM map entries $(vm_entries)"
  fi

  # Copy each new or updated rumble status/session file.
  if [ -d "$STATUS_ROOT" ]; then
    latest=$(ls -t "$STATUS_ROOT" 2>/dev/null | head -1)
    if [ -n "$latest" ]; then
      for f in "$STATUS_ROOT/$latest"/*; do
        [ -f "$f" ] || continue
        key="$latest/$(basename "$f")@$(stat -f %m "$f")"
        case " $seen_status " in *" $key "*) ;; *)
          seen_status="$seen_status $key"
          mkdir -p "$OUT/status/$latest"
          cp "$f" "$OUT/status/$latest/$(date +%H%M%S)-$(basename "$f")"
          say "copied $latest/$(basename "$f")"
        ;; esac
      done
    fi
  fi
  prev=$now
  sleep "$INTERVAL"
done
