"""Render a CoreS3 JSONL walk into the native Corner Walk game project."""

import argparse
from bisect import bisect_left, bisect_right
from collections import Counter
from datetime import datetime
import hashlib
import itertools
import json
import math
import re
import textwrap
import unicodedata
import uuid
from pathlib import Path

from PIL import Image, ImageColor

if __package__:
    from .walk_levels import HEIGHT, INK, PAPER, PAVING, WIDTH, draw_level, make_levels
else:
    from walk_levels import HEIGHT, INK, PAPER, PAVING, WIDTH, draw_level, make_levels

GAME = Path(__file__).resolve().parents[1] / "game"
W, H = WIDTH, HEIGHT
BG_ID = "bebaed2b-eb22-49c4-a626-45f873c07cb6"
MAX_INDOOR_STOPS = 12


def write_resource(path, **changes):
    data = json.loads(path.read_text()) if path.exists() else {}
    data.update(changes)
    path.write_text(json.dumps(data, indent=2) + "\n")


def utc_ms(value):
    if not isinstance(value, str):
        return None
    try:
        stamp = datetime.fromisoformat(value.replace("Z", "+00:00"))
        return round(stamp.timestamp() * 1000) if stamp.tzinfo else None
    except (ValueError, OverflowError):
        return None


def nearest_in_time(timeline, stamp):
    if stamp is None or not timeline:
        return None
    pos = bisect_left(timeline, (stamp, -1))
    when, index = min(timeline[max(0, pos - 1):pos + 1], key=lambda item: abs(item[0] - stamp))
    # The logger normally records once per second. A GPS outage has no known location.
    return index if abs(when - stamp) <= 3000 else None


def coordinates(event):
    lat, lon = event.get("lat"), event.get("lon")
    if all(type(v) in (int, float) for v in (lat, lon)) and -90 <= lat <= 90 and -180 <= lon <= 180:
        return lat, lon
    return None


def place_note(note, tile, signs, moments):
    kind, words = note["type"], note["text"]
    if kind != "said":
        if moments is not None:
            speaker = "Camera" if kind == "scene" else {"user": "You", "assistant": "Guide"}.get(note.get("speaker"), "Chat")
            moments.setdefault(tile, []).append((speaker, words))
        return
    if signs is None:
        return
    match = re.search(r"(?:call|name) (?:this|that|the) (?:corner|spot|place|street)\s+(.+)", words, re.I)
    label = (match[1] if match else words).strip(" .!?\"“”")
    label = unicodedata.normalize("NFKD", label).encode("ascii", "ignore").decode()
    label = " ".join(re.sub(r"[^a-zA-Z0-9 ,.!?'\-]", " ", label).split())[:120]
    if label:
        entries = signs.setdefault(tile, [])
        if label not in entries:
            entries.append(label)


def read_indoor_route(stops, notes, signs, moments, skipped):
    if not 1 <= len(stops) <= MAX_INDOOR_STOPS:
        raise ValueError("An indoor walk needs one to twelve stops")
    clocks = []
    for index, stop in enumerate(stops, start=1):
        stamp = stop.get("walk_ms")
        if stop.get("index") != index or type(stamp) is not int or stamp < 0 or (clocks and stamp <= clocks[-1]):
            raise ValueError("Invalid indoor stop order")
        clocks.append(stamp)
    tiles = [(x, y) for y, columns in ((5, (4, 8, 12, 16)), (10, (16, 12, 8, 4)), (15, (4, 8, 12, 16)))
             for x in columns][:len(stops)]
    for note in notes:
        stamp = note.get("walk_ms")
        if type(stamp) is not int:
            session, offset = note.get("session_ms"), note.get("start_ms", 0)
            if type(session) is int and type(offset) is int:
                stamp = session + offset
        index = bisect_right(clocks, stamp) - 1 if type(stamp) is int else -1
        if index >= 0:
            place_note(note, tiles[index], signs, moments)
        elif skipped is not None:
            skipped[note["type"]] += 1
    x, y = tiles[-1]
    return [(2, 5), *tiles, (x - 2 if y == 10 else x + 2, y)], None


def read_route(path, signs=None, moments=None, skipped=None, *, map_size=(W, H)):
    fixes = []
    notes = []
    clocks, dates, stops = [], [], []
    for line in path.read_text().splitlines():
        if not line.strip():
            continue
        event = json.loads(line)
        if event["type"] == "fix":
            point = coordinates({"lat": float(event["lat"]), "lon": float(event["lon"])})
            if point is None:
                raise ValueError("Invalid GPS coordinate")
            if type(event.get("walk_ms")) is int:
                clocks.append((event["walk_ms"], len(fixes)))
            stamp = utc_ms(event.get("time"))
            if stamp is not None:
                dates.append((stamp, len(fixes)))
            fixes.append(point)
        elif event["type"] == "stop":
            stops.append(event)
        elif event["type"] in ("said", "scene", "chat") and isinstance(event.get("text"), str) and event["text"].strip():
            notes.append(event)
    if stops:
        if fixes:
            raise ValueError("Indoor stops and GPS fixes are separate walks")
        return read_indoor_route(stops, notes, signs, moments, skipped)
    if len(fixes) < 2:
        raise ValueError("A route needs at least two GPS fixes")
    lat0, lon0 = fixes[0]
    meters_lon = 111320 * math.cos(math.radians(lat0))
    points = [((lon - lon0) * meters_lon, (lat0 - lat) * 111320) for lat, lon in fixes]
    distance = sum(math.dist(a, b) for a, b in zip(points, points[1:]))
    xs, ys = zip(*points)
    dx, dy = max(xs) - min(xs), max(ys) - min(ys)
    if max(dx, dy) < 5:
        raise ValueError("Walk farther before building the route (under five meters of extent)")
    width, height = map_size
    scale = min((width - 7) / dx if dx else math.inf, (height - 7) / dy if dy else math.inf)
    mid_x, mid_y = min(xs) + dx / 2, min(ys) + dy / 2
    tiles = [(round((width - 2) / 2 + (x - mid_x) * scale),
              round(height / 2 + (y - mid_y) * scale))
             for x, y in points]
    clocks.sort()
    dates.sort()
    for note in notes:
        kind = note["type"]
        point = coordinates(note)
        nearest = None
        if point is not None:
            lat, lon = point
            position = ((lon - lon0) * meters_lon, (lat0 - lat) * 111320)
            nearest = min(range(len(points)), key=lambda i: math.dist(points[i], position))
        elif type(note.get("walk_ms")) is int and clocks:
            nearest = nearest_in_time(clocks, note["walk_ms"])
        elif kind != "said":
            stamp = utc_ms(note.get("time"))
            if stamp is None and kind == "chat":
                session = utc_ms(note.get("session_time"))
                offset = note.get("start_ms")
                if session is not None and type(offset) is int and offset >= 0 and (offset or note.get("end_ms")):
                    stamp = session + offset
            nearest = nearest_in_time(dates, stamp)
        if nearest is None:
            if skipped is not None:
                skipped[kind] += 1
            continue
        place_note(note, tiles[nearest], signs, moments)
    return [p for p, _ in itertools.groupby(tiles)], distance


def event(event_id, text):
    return {"id": event_id, "command": "EVENT_TEXT", "args": {"text": text}}


def memory_pages(speaker, words):
    punctuation = str.maketrans({"’": "'", "“": '"', "”": '"', "—": "-", "–": "-", "…": "..."})
    words = unicodedata.normalize("NFKD", words.translate(punctuation))
    words = "".join(c if c.isascii() else "?" for c in words if not unicodedata.combining(c))
    lines = [speaker, *textwrap.wrap(" ".join(words.split()), width=18)]
    # Treat transcript punctuation as literal text, including GB Studio control tokens.
    escapes = {"$": r"\005$", "#": r"\005#", "!": r"\005!", "%": "%%", "\\": r"\005\134"}
    return ["\n".join("".join(escapes.get(c, c) for c in line) for line in lines[i:i + 3])
            for i in range(0, len(lines), 3)]


def resource_id(key):
    return str(uuid.uuid5(uuid.NAMESPACE_URL, "corner-walk/memories/" + key))


def memory_input(moments, game, chapter):
    # Read on demand. One input handler avoids consuming the scene's limited
    # trigger slots. Separate native scripts also let long walks span ROM banks.
    scripts = game / "project/scripts"
    checks = []
    for (x, y), entries in sorted(moments.items()):
        key = f"{chapter}_{x}_{y}"
        calls = []
        for offset in range(0, len(entries), 12):
            name = f"walk_memory_{key}_{offset // 12}"
            path = scripts / (name + ".gbsres")
            scripts.mkdir(exist_ok=True)
            script = [event(resource_id(f"{key}/{offset + n}"), memory_pages(speaker, words))
                      for n, (speaker, words) in enumerate(entries[offset:offset + 12])]
            write_resource(path, _resourceType="script", id=resource_id(name), symbol=name,
                           name=f"Walk memory {x},{y} ({offset // 12 + 1})", description="",
                           variables={}, actors={}, script=script)
            calls.append({"id": resource_id("call/" + name), "command": "EVENT_CALL_CUSTOM_EVENT",
                          "args": {"customEventId": resource_id(name)}})
        checks.append({"id": resource_id("at/" + key), "command": "EVENT_IF_ACTOR_AT_POSITION",
                       "args": {"actorId": "player", "units": "tiles", "__disableElse": True,
                                "x": {"type": "number", "value": x}, "y": {"type": "number", "value": y}},
                       "children": {"true": calls}})
    if not checks:
        return []
    return [{"id": resource_id(f"{chapter}/read-button"), "command": "EVENT_SET_INPUT_SCRIPT",
             "args": {"input": ["a"], "override": False}, "children": {"true": checks}}]


SPRITES = {
    "stamp": (
        "..1111..", ".133331.", "13322331", "13233231",
        "13233231", "13322331", ".132231.", "..1111..",
    ),
    "gate": (
        "11111111", "12222221", "12111121", "12133121",
        "12133121", "12111121", "12222221", "11111111",
    ),
    "patrol": (
        "...11...", "..1311..", ".133331.", "11222211",
        "13211231", "13233231", ".111111.", "..1..1..",
    ),
}
STAMP_COUNT, CAUGHT, COMPLETE = "40", "41", "42"
PICKED_UP = ["43", "44", "45", "46"]


def write_sprites(game):
    for name, pixels in SPRITES.items():
        filename = f"walk_{name}.png"
        path = game / "assets/sprites" / filename
        # A single hardware object occupies the lower eight rows of a 16px cel.
        sprite = Image.new("RGB", (16, 16), "#65ff00")
        shades = tuple(map(ImageColor.getrgb, ("#65ff00", INK, PAVING, PAPER)))
        for y, row in enumerate(pixels, start=8):
            for x, code in enumerate(row):
                if code != ".":
                    sprite.putpixel((x, y), shades[int(code)])
        sprite.save(path)
        key = f"sprite/{name}"
        tile = dict(id=resource_id(f"{key}/tile"), x=0, y=0, sliceX=0, sliceY=0, flipX=False,
                    flipY=False, palette=0, paletteIndex=0, objPalette="OBP0", priority=False)
        animations = [{"id": resource_id(f"{key}/animation/{index}"),
                       "frames": [{"id": resource_id(f"{key}/frame/{index}"), "tiles": [tile] if index == 0 else []}]}
                      for index in range(8)]
        write_resource(
            path.with_suffix(".png.gbsres"), _resourceType="sprite", id=resource_id(key),
            name=name.title(), symbol=f"sprite_walk_{name}", filename=filename,
            numTiles=1, width=16, height=16, canvasWidth=16, canvasHeight=16,
            canvasOriginX=0, canvasOriginY=0, boundsX=0, boundsY=0, boundsWidth=8, boundsHeight=8,
            animSpeed=15, checksum=hashlib.sha1(path.read_bytes()).hexdigest(),
            states=[dict(id=resource_id(f"{key}/state"), name="", animationType="fixed",
                         flipLeft=False, animations=animations)],
        )


def number(value):
    return {"type": "number", "value": value}


def position(tile):
    return {"x": number(tile[0]), "y": number(tile[1]), "units": "tiles"}


def command(key, name, *, children=None, **args):
    result = {"id": resource_id(key), "command": name, "args": args}
    if children is not None:
        result["children"] = children
    return result


def if_equal(key, variable, value, yes, no=()):
    condition = {"type": "eq", "valueA": {"type": "variable", "value": variable}, "valueB": number(value)}
    return command(key, "EVENT_IF", condition=condition, __disableElse=not no,
                   children={"true": yes, "false": list(no)})


def write_actor(scene, chapter, name, tile, sprite, index, *, patrol=None):
    key = f"{chapter}/actor/{name}"
    ident = resource_id(key)
    path = scene / "actors" / f"walk_{name}.gbsres"
    path.parent.mkdir(exist_ok=True)
    update = []
    if patrol:
        for leg, destination in enumerate(reversed(patrol)):
            update.extend([
                command(f"{key}/move/{leg}", "EVENT_ACTOR_MOVE_TO", actorId=ident, **position(destination),
                        collideWith=["walls"], moveType="horizontal", lockDirection=[]),
                command(f"{key}/wait/{leg}", "EVENT_WAIT", time=number(0.30), units="time"),
            ])
    write_resource(path, _resourceType="actor", id=ident, _index=index, name=name.replace("_", " ").title(),
                   symbol=f"actor_walk_{chapter}_{name}", prefabId="", prefabScriptOverrides={},
                   coordinateType="tiles", x=tile[0], y=tile[1], direction="right", frame=0, animate=False,
                   spriteSheetId=resource_id(f"sprite/{sprite}"), paletteId="", isPinned=False,
                   moveSpeed=1 if not patrol else 0.5, animSpeed=15, persistent=False,
                   collisionGroup="1" if patrol else "", collisionExtraFlags=[], script=[],
                   startScript=[] if patrol else [command(f"{key}/no-bump", "EVENT_ACTOR_COLLISIONS_DISABLE",
                                                         actorId=ident)],
                   updateScript=update, hit1Script=[], hit2Script=[], hit3Script=[])
    return ident


def write_trigger(scene, chapter, name, tile, script, index):
    path = scene / "triggers" / f"walk_{name}.gbsres"
    path.parent.mkdir(exist_ok=True)
    write_resource(path, _resourceType="trigger", id=resource_id(f"{chapter}/trigger/{name}"), _index=index,
                   symbol=f"trigger_walk_{chapter}_{name}", prefabId="", prefabScriptOverrides={},
                   name=name.replace("_", " ").title(), x=tile[0], y=tile[1], width=1, height=1,
                   script=script, leaveScript=[])


def scene_id(chapter):
    return resource_id(f"scene/{chapter}") if chapter else "12f07015-0a42-504a-8d39-3f689e13ad3a"


def build_level(game, levels, chapter, title, welcome, finish_line, indoor):
    level = levels[chapter]
    size = len(level.stamps)
    prefix = f"level/{chapter}"
    scene_name = f"walk_level_{chapter + 1}" if chapter else "start"
    scene = game / "project/scenes" / scene_name
    scene.mkdir(exist_ok=True)
    for path in (scene / "actors").glob("walk_*.gbsres"):
        path.unlink()
    for path in (scene / "triggers").glob("walk_*.gbsres"):
        path.unlink()
    old_flag = scene / "triggers/finish_flag.gbsres"
    if old_flag.exists() and json.loads(old_flag.read_text()).get("symbol") == "trigger_finish_flag":
        old_flag.unlink()

    bg = game / "assets/backgrounds" / f"walk_chapter_{chapter + 1}.png"
    bg_id = resource_id(f"{prefix}/background")
    draw_level(level, bg, chapter, len(levels), indoor)
    write_resource(bg.with_suffix(".png.gbsres"), _resourceType="background", id=bg_id,
                   name=level.name, symbol=f"bg_walk_{chapter}", tileColors="", filename=bg.name,
                   width=W, height=H, imageWidth=8 * W, imageHeight=8 * H, autoColor=False)
    walls = [0 if (x, y) in level.floor else 15 for y in range(H) for x in range(W)]
    runs = [(value, sum(1 for _ in group)) for value, group in itertools.groupby(walls)]
    collisions = "".join(f"{value:02x}" + ("!" if count == 1 else f"{count:x}+") for value, count in runs)

    gate = write_actor(scene, chapter, "gate", level.exit, "gate", 0)
    patrol = write_actor(scene, chapter, "patrol", level.patrol[0], "patrol", 1, patrol=level.patrol)
    for index, (tile, variable) in enumerate(zip(level.stamps, PICKED_UP)):
        key = f"{prefix}/stamp/{index}"
        actor = write_actor(scene, chapter, f"stamp_{index + 1}", tile, "stamp", index + 2)
        collect = [
            command(f"{key}/mark", "EVENT_SET_VALUE", variable=variable, value=number(1)),
            command(f"{key}/count", "EVENT_INC_VALUE", variable=STAMP_COUNT),
            command(f"{key}/hide", "EVENT_ACTOR_HIDE", actorId=actor),
            command(f"{key}/sound", "EVENT_SOUND_PLAY_EFFECT", type="beep", pitch=6, duration=0.10),
            if_equal(f"{key}/open", STAMP_COUNT, size, [
                command(f"{key}/hide-gate", "EVENT_ACTOR_HIDE", actorId=gate),
                command(f"{key}/unlock", "EVENT_SOUND_PLAY_EFFECT", type="tone", frequency=760, duration=0.24),
            ]),
        ]
        write_trigger(scene, chapter, f"stamp_{index + 1}", tile,
                      [if_equal(f"{key}/available", variable, 0, collect)], index)

    if chapter + 1 < len(levels):
        cleared = [command(f"{prefix}/advance", "EVENT_SWITCH_SCENE", sceneId=scene_id(chapter + 1),
                           **position(levels[chapter + 1].start), direction="down", fadeSpeed="2")]
    else:
        victory = [
            command(f"{prefix}/complete", "EVENT_SET_VALUE", variable=COMPLETE, value=number(1)),
            command(f"{prefix}/stop-patrol", "EVENT_ACTOR_HIDE", actorId=patrol),
            if_equal(f"{prefix}/rank", CAUGHT, 0,
                     [event(resource_id(f"{prefix}/clean"), f"{finish_line}\nClean run!\nRoute complete")],
                     [event(resource_id(f"{prefix}/finish"), f"{finish_line}\nCaught: $41$\nRoute complete")]),
        ]
        cleared = [if_equal(f"{prefix}/first-finish", COMPLETE, 0, victory)]
    locked = event(resource_id(f"{prefix}/locked"), f"Gate locked\nStamps: $40$/{size}\nFind them all.")
    write_trigger(scene, chapter, "exit", level.exit,
                  [if_equal(f"{prefix}/can-exit", STAMP_COUNT, size, cleared, [locked])], size)

    reset = [STAMP_COUNT, *PICKED_UP] + ([] if chapter else [CAUGHT, COMPLETE])
    events = [
        command(f"{prefix}/player-bounds", "EVENT_ACTOR_SET_COLLISION_BOX",
                actorId="player", x=number(0), y=number(0), width=number(8), height=number(8)),
        *[command(f"{prefix}/reset/{var}", "EVENT_SET_VALUE", variable=var, value=number(0)) for var in reset],
    ]
    if not chapter:
        pages = [f"CORNER WALK\n{title}\n{welcome}", "Collect stamps.\nReach the gate.\nB shows progress.",
                 "Dodge the drone.\nTake the long way\nfor a safer route."]
        if any(stage.memories for stage in levels):
            pages.append("Stand on a marker.\nA reads a clue.")
        events.append(event(resource_id(f"{prefix}/intro"), pages))
    events.extend(memory_input(level.memories, game, chapter))
    stats = event(resource_id(f"{prefix}/stats"), f"Stamps: $40$/{size}\nCaught: $41$\n{level.name}")
    events.append(command(f"{prefix}/status-button", "EVENT_SET_INPUT_SCRIPT", input=["b"], override=False,
                          children={"true": [stats]}))
    hit = [if_equal(f"{prefix}/caught", COMPLETE, 0, [
        command(f"{prefix}/count-hit", "EVENT_INC_VALUE", variable=CAUGHT),
        command(f"{prefix}/hit-sound", "EVENT_SOUND_PLAY_EFFECT", type="crash", duration=0.14),
        command(f"{prefix}/respawn", "EVENT_ACTOR_SET_POSITION", actorId="player", **position(level.start)),
        command(f"{prefix}/jolt", "EVENT_CAMERA_SHAKE", shakeDirection="horizontal", units="frames",
                frames=12, magnitude=number(2)),
    ])]
    palette = (("default-bg-5", "default-bg-3", "default-bg-4") if indoor
               else ("default-bg-6", "default-bg-1", "default-bg-5"))[chapter]
    write_resource(scene / "scene.gbsres", _resourceType="scene", id=scene_id(chapter), type="TOPDOWN",
                   name=level.name, symbol=f"scene_walk_{chapter}", _index=chapter, x=chapter * 184, y=0,
                   backgroundId=bg_id, tilesetId="", width=W, height=H, colorModeOverride="none",
                   paletteIds=[palette], spritePaletteIds=[], autoFadeSpeed=1, collisions=collisions,
                   script=events, playerHit1Script=hit, playerHit2Script=[], playerHit3Script=[])


def build_project(walk, game=GAME, game_copy=None, *, map_size=(W, H)):
    signs, moments, skipped = {}, {}, Counter()
    points, distance = read_route(walk, signs, moments, skipped, map_size=map_size)
    levels = make_levels(points, distance, signs, moments)
    copy = game_copy or dict(title="Indoor route" if distance is None else "Your walk",
                             welcome="Stamp rally", finish="All stamps found")
    write_level_project(game, levels, copy, indoor=distance is None)
    print(f"{copy['title']}: levels={len(levels)}, stamps={sum(len(level.stamps) for level in levels)}.")
    print(f"{sum(map(len, moments.values()))} camera observations and chat captions mapped to walk stops.")
    if skipped:
        print("Unplaced: " + ", ".join(f"{n} {kind}" for kind, n in sorted(skipped.items())))


def write_level_project(game, levels, game_copy, *, indoor=False):
    project = game / "project"
    for path in (project / "scripts").glob("walk_memory_*.gbsres"):
        if re.fullmatch(r"walk_memory_(?:\d+_){2,3}\d+", path.stem):
            path.unlink()
    # Previous generated projects can be rebuilt in place, including shorter walks.
    for chapter in range(len(levels), 3):
        stale = project / "scenes" / f"walk_level_{chapter + 1}" / "scene.gbsres"
        if stale.exists() and json.loads(stale.read_text()).get("id") == scene_id(chapter):
            for kind in ("actors", "triggers"):
                for resource in (stale.parent / kind).glob("walk_*.gbsres"):
                    resource.unlink()
            stale.unlink()
    variables_file = project / "variables.gbsres"
    variables = json.loads(variables_file.read_text())["variables"]
    names = dict(zip([STAMP_COUNT, CAUGHT, COMPLETE, *PICKED_UP],
                     ["Level stamps", "Times caught", "Route complete", *[f"Stamp {n} collected" for n in range(1, 5)]]))
    variables = [entry for entry in variables if entry["id"] not in names]
    variables.extend(dict(id=ident, name=name, symbol=f"var_walk_{ident}") for ident, name in names.items())
    write_resource(variables_file, variables=variables)
    write_sprites(game)
    for chapter in range(len(levels)):
        build_level(game, levels, chapter, game_copy["title"], game_copy["welcome"], game_copy["finish"], indoor)
    write_resource(project / "settings.gbsres", startSceneId=scene_id(0),
                   startX=levels[0].start[0], startY=levels[0].start[1], startDirection="down")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("walk", type=Path)
    parser.add_argument("--project", type=Path, default=GAME, help="Native GB Studio project directory")
    args = parser.parse_args()
    build_project(args.walk, args.project)


if __name__ == "__main__":
    main()
