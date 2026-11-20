#!/usr/bin/env python3
"""Match a weapon_capture.sh timeline against an onfoot-events capture.

A weapon's window runs from its spoken "fire" to the next "get ready" (so
shots fired past "stop" still count). Background is everything posted during
the quiet baseline and the "get ready" periods. For each weapon this lists the
Wwise event IDs absent from the background, checks every ID against the ship
weapon routes in include/edtrace/ship_weapon_routes.hpp (family, size, cue
role, LOD), and reports which existing mappings matched and whether the sound
actually played (returned_playing_id != 0).

Usage:
  tools/weapon_capture_report.py <capture dir with timeline.tsv> <onfoot-events-*.jsonl> [--json out.json]
Output is Markdown on stdout.
"""
import argparse
import collections
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
ROUTES = ROOT / "include/edtrace/ship_weapon_routes.hpp"


def load_routes():
    text = ROUTES.read_text()
    known = {}
    pattern = re.compile(r"\{ShipWeaponFamily::(\w+),\s*ShipWeaponSize::(\w+),\s*\{\{(\d+)u?,\s*(\d+)u?,\s*(\d+)u?\},\s*\{(\d+)u?,\s*(\d+)u?,\s*(\d+)u?\}\}\}")
    for family, size, *ids in pattern.findall(text):
        for lod in range(2):
            for role, value in zip(("start", "stop", "shot"), ids[lod * 3:lod * 3 + 3]):
                if int(value):
                    known[int(value)] = f"{family}/{size} {role} LOD{lod}"
    return known


def load_timeline(path):
    rows = [line.rstrip("\n").split("\t") for line in path.read_text().splitlines()[1:] if line.strip()]
    marks = [(int(ms), event) for ms, _, event in rows]
    start = next(ms for ms, e in marks if e.startswith("recording started"))
    end = next((ms for ms, e in marks if e == "marker removed"), marks[-1][0])
    windows, quiet = [], []
    for i, (ms, event) in enumerate(marks):
        following = marks[i + 1][0] if i + 1 < len(marks) else end
        if event.startswith("fire "):
            later = [m for m, e in marks[i + 1:] if e.startswith("ready ") or e == "marker removed"]
            windows.append((event[5:], ms, later[0] if later else end))
        elif event.startswith("ready ") or event == "baseline quiet start":
            quiet.append((ms, following))
    return windows, quiet, start, end


def load_posts(path):
    posts, session = [], None
    with open(path) as handle:
        for line in handle:
            try:
                record = json.loads(line)
            except ValueError:
                continue
            if record.get("type") == "onfoot_event_session":
                session = record
            elif record.get("type") == "onfoot_queue_post":
                posts.append(record)
    if not session:
        sys.exit("capture has no session header")
    ticks = 10_000_000 / session["qpc_frequency"]  # FILETIME ticks per QPC tick

    def epoch_ms(qpc):
        filetime = session["time_anchor_filetime"] + (qpc - session["time_anchor_qpc"]) * ticks
        return filetime / 10_000 - 11_644_473_600_000
    for post in posts:
        post["ms"] = epoch_ms(post["start_qpc"])
    return session, posts


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("capture_dir", type=pathlib.Path)
    parser.add_argument("events", type=pathlib.Path)
    parser.add_argument("--json", type=pathlib.Path)
    args = parser.parse_args()

    known = load_routes()
    windows, quiet, start, end = load_timeline(args.capture_dir / "timeline.tsv")
    session, posts = load_posts(args.events)
    posts = [p for p in posts if start <= p["ms"] <= end]

    def window_of(ms):
        for name, begin, stop in windows:
            if begin <= ms < stop:
                return name
        return None

    background = collections.Counter(p["event_id"] for p in posts
                                     if any(begin <= p["ms"] < stop for begin, stop in quiet))
    per_weapon = {name: [p for p in posts if window_of(p["ms"]) == name] for name, _, _ in windows}
    weapons_of_id = collections.defaultdict(set)
    for name, items in per_weapon.items():
        for p in items:
            weapons_of_id[p["event_id"]].add(name)

    out = {"game_sha256": session.get("verified_game_sha256"), "weapons": {}}
    print("# Ship weapon capture report\n")
    print(f"Game build SHA-256 `{session.get('verified_game_sha256')}`. "
          f"{len(posts)} queue posts during the capture; {sum(background.values())} in the quiet/ready periods.\n")
    print("A weapon's window runs from its spoken *fire* to the next *get ready*. Listed IDs never occur in the "
          "quiet or ready periods. *Shared with* = also posted in another weapon's window. "
          "*Played* = posts whose returned_playing_id was non-zero.\n")
    for name, begin, stop in windows:
        items = per_weapon[name]
        counts = collections.Counter(p["event_id"] for p in items)
        print(f"## {name}\n")
        rows = []
        for event_id, count in counts.most_common():
            if background[event_id]:
                continue
            first = min(p["ms"] for p in items if p["event_id"] == event_id) - begin
            objects = sorted({p["game_object_id"] for p in items if p["event_id"] == event_id})
            callers = sorted({p["caller_rva"] for p in items if p["event_id"] == event_id})
            others = sorted(weapons_of_id[event_id] - {name})
            played = sum(1 for p in items if p["event_id"] == event_id and p["returned_playing_id"])
            rows.append({"event_id": event_id, "count": count, "played": played, "first_ms": round(first),
                         "objects": len(objects), "caller_rvas": [hex(c) for c in callers],
                         "known": known.get(event_id), "shared_with": others})
        if not rows:
            print("No weapon-specific events: nothing posted that is absent from the quiet periods.\n")
        else:
            print("| event id | posts | played | first at | objects | caller RVA | mod mapping | shared with |")
            print("|---:|---:|---:|---:|---:|---|---|---|")
            for r in rows:
                print(f"| {r['event_id']} | {r['count']} | {r['played']} | +{r['first_ms']} ms | {r['objects']} | "
                      f"{', '.join(r['caller_rvas'])} | {r['known'] or '**not mapped**'} | {', '.join(r['shared_with']) or ''} |")
            matched = [r for r in rows if r["known"]]
            print(f"\n{len(rows)} weapon-window event IDs, {len(matched)} already mapped by the mod.\n")
        out["weapons"][name] = {"fire_ms": begin, "stop_ms": stop, "events": rows}
    if args.json:
        args.json.write_text(json.dumps(out, indent=1))


if __name__ == "__main__":
    main()
