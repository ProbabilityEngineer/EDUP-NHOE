# Sourced by the Mac CrossOver tools. Picks the bottle to use: an explicit
# name (--bottle), else $EDUP_BOTTLE, else the only bottle with Elite
# Dangerous installed. With several such bottles it lists them and fails.
EDUP_BOTTLES_DIR="$HOME/Library/Application Support/CrossOver/Bottles"
EDUP_GAME_SUBDIR="drive_c/Program Files (x86)/Frontier/EDLaunch/Products/elite-dangerous-odyssey-64"
edup_bottle() {
  local requested="${1:-${EDUP_BOTTLE:-}}" found=() bottle
  if [ -n "$requested" ]; then echo "$requested"; return 0; fi
  for bottle in "$EDUP_BOTTLES_DIR"/*/; do
    [ -d "$bottle$EDUP_GAME_SUBDIR" ] && found+=("$(basename "$bottle")")
  done
  if [ "${#found[@]}" -eq 1 ]; then echo "${found[0]}"; return 0; fi
  if [ "${#found[@]}" -eq 0 ]; then echo "No CrossOver bottle with Elite Dangerous found." >&2
  else printf 'Several bottles have Elite Dangerous; pass --bottle NAME or set EDUP_BOTTLE:\n' >&2
       printf '  %s\n' "${found[@]}" >&2; fi
  return 1
}
