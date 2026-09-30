#!/usr/bin/env python3
"""Validate a scene graph: the JSON Schema, then the rules of scene_graph_spec.md §6.

The engine checks the same rules itself (core/src/scene.cpp) when a scene is opened.

    scripts/validate_scene.py scene.json [...]

The schema check needs the `jsonschema` package; without it only §6 is checked.
Exit status is 1 if any file has errors.
"""

import json
import pathlib
import sys
from fractions import Fraction

SCHEMA = pathlib.Path(__file__).resolve().parent.parent / "schema" / "scene_graph.schema.json"
EPSILON = Fraction(1, 1000000)  # times are compared to the microsecond

# Allowed ranges for animatable values (§4.3), by where the value sits.
RANGES = {
    "opacity": (0, 1), "scale": (0.0001, 100), "gain": (0, 4), "pan": (-1, 1),
    "brightness": (-1, 1), "contrast": (0, 2), "saturation": (0, 2),
    "radius": (0, 0.1), "left": (0, 1), "top": (0, 1), "right": (0, 1), "bottom": (0, 1),
}
# Any other effect type comes from an effect plugin: its parameters' ranges are known only to the
# engine, once the plugin is loaded.
BUILTIN_EFFECTS = ("colorAdjust", "blur", "crop", "chromaKey")


def to_time(value):
    if isinstance(value, str):
        num, den = value.split("/")
        return Fraction(int(num), int(den))
    return Fraction(value).limit_denominator(1000000)


def keyframes(value):
    """The [(time, value)] of an animatable, or None for a constant."""
    if isinstance(value, dict):
        return [(to_time(k[0]), k[1]) for k in value["keys"]]
    return None


def to_end(item):
    """A video or audio item with duration 0 plays to the end of its file, unknown here."""
    return item["type"] in ("video", "audio") and to_time(item["duration"]) == 0


def check_animatable(name, value, duration, where, errors, ranged=True):
    lo, hi = RANGES.get(name, (None, None)) if ranged else (None, None)
    keys = keyframes(value)
    values = [value] if keys is None else [v for _, v in keys]
    if lo is not None:
        for v in values:
            if not lo <= v <= hi:
                errors.append(f"{where}: {name} {v} is outside [{lo}, {hi}]")
    if keys:
        times = [t for t, _ in keys]
        if any(b <= a for a, b in zip(times, times[1:])):
            errors.append(f"{where}: {name} key times must increase")
        if times[0] < 0 or (duration is not None and times[-1] > duration + EPSILON):
            errors.append(f"{where}: {name} keys must lie within the item (0 to {float(duration)} s)")


def check_item(item, where, errors):
    duration = None if to_end(item) else to_time(item["duration"])  # None: checked by the engine once probed
    for name in ("opacity", "gain", "pan"):
        if name in item:
            check_animatable(name, item[name], duration, where, errors)
    for name, value in item.get("transform", {}).items():
        if name in ("x", "y", "scale", "rotation"):
            check_animatable(name, value, duration, f"{where}.transform", errors)
    for name in ("gain", "pan"):
        if name in item.get("audio", {}):
            check_animatable(name, item["audio"][name], duration, f"{where}.audio", errors)
    check_effects(item, duration, where, errors)


def check_effects(node, duration, where, errors):
    """At most one effect of each type (R12), values in range (R7), keys within the item (R6)."""
    types = [e["type"] for e in node.get("effects", [])]
    for t in set(types):
        if types.count(t) > 1:
            errors.append(f"{where}.effects: at most one {t} effect (R12)")
    for i, effect in enumerate(node.get("effects", [])):
        builtin = effect["type"] in BUILTIN_EFFECTS
        for name, value in effect.items():
            if name != "type" and not isinstance(value, str) and name not in ("tolerance", "softness"):
                check_animatable(name, value, duration, f"{where}.effects[{i}]", errors, ranged=builtin)
        if effect["type"] == "crop":
            if keyframes(effect.get("left", 0)) is None and keyframes(effect.get("right", 0)) is None:
                if effect.get("left", 0) + effect.get("right", 0) >= 1:
                    errors.append(f"{where}.effects[{i}]: crop removes the whole width")
            if keyframes(effect.get("top", 0)) is None and keyframes(effect.get("bottom", 0)) is None:
                if effect.get("top", 0) + effect.get("bottom", 0) >= 1:
                    errors.append(f"{where}.effects[{i}]: crop removes the whole height")


def check_track(track, t_index, errors):
    name = f"tracks[{t_index}]" + (f" ({track['id']})" if "id" in track else "")
    check_effects(track, None, name, errors)  # scene time: keys from 0 on
    items = track["items"]
    previous = None  # the last non-transition item
    pending = None   # a transition waiting for the item after it
    for i, item in enumerate(items):
        where = f"{name}.items[{i}]"
        if item["type"] == "transition":
            if previous is None or pending is not None:
                errors.append(f"{where}: a transition must sit between two items")
            else:
                pending = (item, where)
            continue
        check_item(item, where, errors)
        start, end = to_time(item["start"]), to_time(item["start"]) + to_time(item["duration"])
        if previous is not None:
            prev_item, prev_where = previous
            prev_start = to_time(prev_item["start"])
            prev_end = prev_start + to_time(prev_item["duration"])
            if start < prev_start:
                errors.append(f"{where}: items must be in start order")
            elif to_end(prev_item):
                pass  # where it ends is known once the engine probes its file
            elif pending is None:
                if start < prev_end - EPSILON:
                    errors.append(f"{where}: overlaps {prev_where} without a transition between them")
            else:
                transition, t_where = pending
                d = to_time(transition["duration"])
                if transition["kind"] == "cut" and d != 0:
                    errors.append(f"{t_where}: a cut has duration 0")
                if abs(start - (prev_end - d)) > EPSILON:
                    errors.append(f"{t_where}: the items around it must overlap by exactly {float(d)} s "
                                  f"(next item starts at {float(start)}, expected {float(prev_end - d)})")
                limit = (prev_end - prev_start if to_end(item) else min(prev_end - prev_start, end - start)) / 2
                if d > limit + EPSILON:
                    errors.append(f"{t_where}: {float(d)} s is longer than half of an item it joins ({float(limit)} s)")
        previous, pending = (item, where), None
    if pending is not None:
        errors.append(f"{pending[1]}: a transition must sit between two items")


def check_rules(doc):
    errors = []
    ids = {}

    def collect(node, where):
        if isinstance(node, dict):
            if isinstance(node.get("id"), str):
                if node["id"] in ids:
                    errors.append(f"{where}: id {node['id']!r} is also used at {ids[node['id']]}")
                ids.setdefault(node["id"], where)
            for key, value in node.items():
                if key != "metadata":
                    collect(value, f"{where}.{key}")
        elif isinstance(node, list):
            for i, value in enumerate(node):
                collect(value, f"{where}[{i}]")

    collect(doc, "$")
    for t, track in enumerate(doc["tracks"]):
        check_track(track, t, errors)
    if not any(item["type"] != "transition" for track in doc["tracks"] if track.get("enabled", True)
               for item in track["items"]):
        errors.append("$: nothing to play: every enabled track is empty")
    return errors


def main(paths):
    try:
        import jsonschema
        validator = jsonschema.Draft202012Validator(json.loads(SCHEMA.read_text()))
    except ImportError:
        validator = None
        print("note: jsonschema is not installed; checking §6 rules only", file=sys.stderr)
    failed = False
    for path in paths:
        doc = json.loads(pathlib.Path(path).read_text())
        errors = []
        if validator:
            from jsonschema.exceptions import best_match
            for e in sorted(validator.iter_errors(doc), key=lambda e: list(e.absolute_path)):
                leaf = best_match(e.context) if e.context else e  # the branch of a oneOf that came closest
                errors.append(f"schema: {'/'.join(map(str, leaf.absolute_path)) or '$'}: {leaf.message}")
        if not errors:  # the rules assume a structurally valid document
            errors = check_rules(doc)
        if errors:
            failed = True
            print(f"{path}: {len(errors)} error(s)")
            for e in errors:
                print(f"  {e}")
        else:
            end = max((to_time(i["start"]) + to_time(i["duration"]) for tr in doc["tracks"] for i in tr["items"]
                       if i["type"] != "transition"), default=0)
            open_ended = any(to_end(i) for tr in doc["tracks"] for i in tr["items"] if i["type"] != "transition")
            print(f"{path}: ok ({len(doc['tracks'])} tracks, {'at least ' if open_ended else ''}{float(end):g} s)")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]) if len(sys.argv) > 1 else print(__doc__) or 2)
