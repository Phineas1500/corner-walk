import json
import tempfile
import unittest
from collections import Counter, deque
from pathlib import Path

from tools.export_walk import split_walks
from tools.make_walk_game import memory_pages, read_route
from tools.walk_levels import make_levels


class WalkMemoryTests(unittest.TestCase):
    def test_every_indoor_length_has_a_safe_route_and_preserves_each_stop(self):
        for stop_count in range(1, 13):
            route = [(x, 5) for x in range(stop_count + 2)]
            observations = {tile: [("Camera", str(tile))] for tile in route[1:-1]}
            levels = make_levels(route, None, {}, observations)
            placed = [entry for level in levels for entries in level.memories.values() for entry in entries]
            self.assertEqual(placed, [entry for entries in observations.values() for entry in entries])
            for level in levels:
                (left, y), (right, end_y) = level.patrol
                self.assertEqual(y, end_y)
                track = {(x, y) for x in range(min(left, right), max(left, right) + 1)}
                self.assertTrue(track <= level.floor)
                reached = {level.start}
                queue = deque(reached)
                while queue:
                    x, y = queue.popleft()
                    for tile in ((x, y - 1), (x + 1, y), (x, y + 1), (x - 1, y)):
                        if tile in level.floor and tile not in track and tile not in reached:
                            reached.add(tile)
                            queue.append(tile)
                self.assertTrue({level.exit, *level.stamps, *level.memories} <= reached, level.name)

    def test_a_gps_loop_retains_landmarks_without_stacking_close_stops(self):
        route = [(x, 5) for x in range(9, -1, -1)]
        signs = {point: [str(point)] for point in route}
        [level] = make_levels(route + route[::-1], 100, signs, {})
        self.assertEqual(len(level.memories), len(signs))
        self.assertEqual([entries[0] for entries in level.memories.values()],
                         [("Landmark", str(point)) for point in route])
        self.assertGreater(level.start[0], level.exit[0])

    def test_indoor_captions_follow_stop_time_instead_of_export_order(self):
        rows = [
            {"type": "start"},
            {"type": "stop", "index": 1, "walk_ms": 100},
            {"type": "scene", "walk_ms": 225, "text": "A red mug"},
            {"type": "stop", "index": 2, "walk_ms": 500},
            {"type": "chat", "walk_ms": 320, "speaker": "user", "text": "What color is it?"},
            {"type": "chat", "walk_ms": 560, "speaker": "assistant", "text": "Red."},
            {"type": "said", "walk_ms": 600, "text": "Name this spot Desk"},
        ]
        log = "".join(json.dumps(row) + "\n" for row in rows).encode()
        self.assertEqual(len(split_walks(log)), 1)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "walk.jsonl"
            path.write_bytes(log)
            signs, memories = {}, {}
            tiles, distance = read_route(path, signs, memories)
        self.assertIsNone(distance)
        self.assertEqual(memories[tiles[1]], [("Camera", "A red mug"), ("You", "What color is it?")])
        self.assertEqual(memories[tiles[2]], [("Guide", "Red.")])
        self.assertEqual(signs[tiles[2]], ["Desk"])

    def test_moving_chat_uses_utterance_time_and_skips_gps_gaps(self):
        start = "2026-11-19T18:00:00Z"
        route = [
            {"type": "fix", "walk_ms": 5000 + t * 1000, "time": start[:17] + f"{t:02d}Z",
             "lat": 40, "lon": -74 + t * .00001} for t in (0, 10, 20)
        ]
        # Captions arrive together in the dump after the walker's position changed.
        old_chat = {"type": "chat", "session_time": start, "speaker": "user", "end_ms": 21000}
        rows = route + [
            {"type": "scene", "lat": 40, "lon": -74, "text": "A mural"},
            dict(old_chat, walk_ms=15100, start_ms=500, text="At the middle"),
            dict(old_chat, start_ms=20100, text="At the end"),
            dict(old_chat, walk_ms=20000, start_ms=10, text="GPS dropped"),
            {"type": "chat", "session_time": start, "text": "Missing turn timestamp"},
        ]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "walk.jsonl"
            path.write_text("\n".join(map(json.dumps, rows)))
            memories, skipped = {}, Counter()
            tiles, _ = read_route(path, moments=memories, skipped=skipped)
        self.assertEqual([memories[tile] for tile in tiles],
                         [[("Camera", "A mural")], [("You", "At the middle")], [("You", "At the end")]])
        self.assertEqual(skipped, {"chat": 2})

    def test_long_caption_keeps_its_last_words_on_readable_pages(self):
        words = "Look at the tall trees by the river. " * 7 + "Two herons!"
        pages = memory_pages("Guide", words)
        self.assertGreater(len(pages), 1)
        lines = [line for page in pages for line in page.splitlines()]
        self.assertTrue(all(len(page.splitlines()) <= 3 for page in pages))
        self.assertEqual(lines[0], "Guide")
        self.assertEqual(" ".join(lines[1:]).replace(r"\005!", "!"), words)


if __name__ == "__main__":
    unittest.main()
