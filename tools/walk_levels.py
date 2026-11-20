"""Design and draw the short checkpoint levels in a generated Corner Walk."""

from collections import deque
from dataclasses import dataclass
from math import ceil

from PIL import Image, ImageDraw

WIDTH, HEIGHT = 20, 18
PAPER, PAVING, BRICK, INK = "#e0f8cf", "#86c06c", "#306850", "#071821"

# The dashed shortcut runs between p and q. Each room also has a safe detour.
# Stamps a–d are ordered along a suggested route. Players can collect in any order.
LAYOUTS = (
    (
        "####################",
        "####################",
        "#.....#............#",
        "#..a..#.........c..#",
        "#.....#..######....#",
        "#.....#..#....#....#",
        "#.....#..#....#....#",
        "#.....#..#....#....#",
        "#..@.....#....#....#",
        "#.....p......q.....#",
        "#.....#..#....#....#",
        "#.....#..######....#",
        "#.....#............#",
        "#.....#....d.......#",
        "#.....#............#",
        "#.......b........E.#",
        "#..................#",
        "####################",
    ),
    (
        "####################",
        "####################",
        "#....d...........E.#",
        "#..b.#####.........#",
        "#....#...#.........#",
        "#....#...######....#",
        "#........#.........#",
        "#....#...#.........#",
        "#....#...#....c....#",
        "#....#...#.........#",
        "#....#...#.........#",
        "#....#...#.........#",
        "#....p........q....#",
        "#....#...###.#.....#",
        "#....#.......#.....#",
        "#..@.#......a#.....#",
        "#..................#",
        "####################",
    ),
    (
        "####################",
        "####################",
        "#..E.........#.....#",
        "#.d..........#..a..#",
        "#....p.......q.....#",
        "#....#.......#.....#",
        "#....#.......#.....#",
        "#....######..#.....#",
        "#.........#..#..@..#",
        "#.........#..#.....#",
        "#.......c.#..#.....#",
        "#.........#..#.....#",
        "#....######..#.....#",
        "#....#.......#.....#",
        "#....#.......#.....#",
        "#....#..........b..#",
        "#..................#",
        "####################",
    ),
)
INDOOR_NAMES = ("Lobby", "Switchback", "Service hall")
OUTDOOR_NAMES = ("Courtyard", "Side streets", "Overpass")


@dataclass
class Level:
    name: str
    floor: set
    start: tuple
    exit: tuple
    patrol: tuple
    stamps: list
    memories: dict


def nearby_clue_tile(anchor, safe, occupied):
    """Keep successive recorded locations distinct within a stamp's area."""
    queue = deque([anchor])
    seen = {anchor}
    while queue:
        tile = queue.popleft()
        if tile not in occupied:
            return tile
        x, y = tile
        for neighbor in ((x, y - 1), (x + 1, y), (x, y + 1), (x - 1, y)):
            if neighbor in safe and neighbor not in seen:
                seen.add(neighbor)
                queue.append(neighbor)
    return anchor


def make_levels(points, distance, signs, moments, *, layouts=None):
    indoor = distance is None
    route = points[1:-1] if indoor else points
    if layouts is None:
        count = ceil(len(route) / 4) if indoor else 1 + (distance >= 160) + (distance >= 600)
        layouts = range(min(count, len(LAYOUTS), len(route)))
    count = len(layouts)
    # Revisited GPS cells share a marker. Put it at the first visit.
    locations = {}
    for index, point in enumerate(route):
        locations.setdefault(point, index)
    names = INDOOR_NAMES if indoor else OUTDOOR_NAMES
    levels = []
    for index, layout in enumerate(layouts):
        first, end = index * len(route) // count, (index + 1) * len(route) // count
        start, finish = route[first], route[end - 1]
        if finish[0] == start[0]:
            finish = max(route[first:end], key=lambda tile: abs(tile[0] - start[0]))
        mirror = finish[0] < start[0]
        rows = LAYOUTS[layout]
        floor = {(x, y) for y, row in enumerate(rows) for x, char in enumerate(row) if char != "#"}
        markers = {char: (x, y) for y, row in enumerate(rows)
                   for x, char in enumerate(row) if char not in "#."}
        if mirror:
            floor = {(WIDTH - 1 - x, y) for x, y in floor}
            markers = {char: (WIDTH - 1 - x, y) for char, (x, y) in markers.items()}
        # Indoor sections have one slot per stop (with a two-stamp minimum).
        # GPS sections sample the route in order so dense fixes don't add chores.
        slots = min(4 if indoor else 3, max(2, end - first))
        stamps = [markers[char] for char in "abcd"[:slots]]
        clues = {}
        (ax, ay), (bx, _) = markers["p"], markers["q"]
        safe = floor - {(x, ay) for x in range(min(ax, bx), max(ax, bx) + 1)}
        reserved = {markers["@"], markers["E"], *stamps}
        for position, point in enumerate(route[first:end], start=first):
            if locations[point] != position or (point not in signs and point not in moments):
                continue
            slot = round((position - first) * (slots - 1) / max(1, end - first - 1))
            anchor = stamps[slot]
            tile = nearby_clue_tile(anchor, safe, reserved | clues.keys()) if anchor in clues else anchor
            clues.setdefault(tile, []).extend(
                [("Landmark", label) for label in signs.get(point, [])] + moments.get(point, []))
        levels.append(Level(names[layout], floor, markers["@"], markers["E"],
                            (markers["p"], markers["q"]), stamps, clues))
    return levels


def draw_level(level, path, index, total, indoor):
    board = Image.new("RGB", (8 * WIDTH, 8 * HEIGHT), BRICK)
    pen = ImageDraw.Draw(board)
    for y in range(2, HEIGHT):
        for x in range(WIDTH):
            px, py = x * 8, y * 8
            if (x, y) in level.floor:
                pen.rectangle((px, py, px + 7, py + 7), fill=PAPER)
                if (x + y) % 3 == 0:
                    pen.point((px + 2, py + 6), fill=PAVING)
                if (x, y + 1) not in level.floor:
                    pen.line((px, py + 7, px + 7, py + 7), fill=PAVING)
            elif indoor:
                pen.line((px, py + 1, px + 7, py + 1), fill=PAVING)
                seam = px + 4 * (y % 2)
                pen.line((seam, py + 1, seam, py + 7), fill=INK)
            else:
                pen.rectangle((px + 1, py + 1, px + 6, py + 6), outline=INK, fill=BRICK)
                pen.line((px + 2, py + 2, px + 5, py + 2), fill=PAVING)
    (ax, ay), (bx, _) = level.patrol
    for x in range(min(ax, bx), max(ax, bx) + 1):
        for offset in (0, 5):
            pen.line((x * 8 + 2, ay * 8 + offset, x * 8 + 5, ay * 8 + offset), fill=BRICK)
    for x, y in dict.fromkeys([*level.stamps, *level.memories]):
        marked = (x, y) in level.memories
        px, py = x * 8, y * 8
        pen.rectangle((px, py, px + 7, py + 7), outline=BRICK if marked else PAVING)
        if marked:
            pen.line((px + 2, py + 6, px + 5, py + 6), fill=INK)
            pen.rectangle((px + 2, py + 2, px + 5, py + 4), fill=BRICK)
    for (x, y), shade in ((level.start, PAVING), (level.exit, BRICK)):
        px, py = x * 8, y * 8
        pen.rectangle((px, py, px + 7, py + 7), fill=shade, outline=INK)
        pen.line((px + 2, py + 4, px + 5, py + 4), fill=PAPER)
    pen.rectangle((0, 0, 159, 14), fill=INK)
    pen.line((0, 15, 159, 15), fill=PAVING)
    mask = Image.new("1", board.size)
    labels = ImageDraw.Draw(mask)
    labels.text((4, 1), f"{index + 1}/{total}  {level.name.upper()}", fill=1)
    hint = "B STATS"
    labels.text((8 * WIDTH - 3 - round(labels.textlength(hint)), 1), hint, fill=1)
    board.paste(PAPER, (0, 0, *board.size), mask)
    board.save(path)
