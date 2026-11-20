"""Receive one finished walk through the authenticated tunnel and build its ROM."""

import argparse
import errno
import hmac
import json
import os
import re
import shutil
import subprocess
import tempfile
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from relay.game_copy import make_copy
from relay.live import private_key
from tools.export_walk import split_walks
from tools.make_walk_game import GAME, build_project, read_route

ROOT = Path(__file__).resolve().parents[1]
LIMIT = 2 * 1024 * 1024
WALK_PATH = re.compile(r"/walk/([a-f0-9]{32})")


def atomic_write(path, content):
    draft = path.with_suffix(".new")
    with draft.open("wb") as output:
        output.write(content)
        output.flush()
        os.fsync(output.fileno())
    draft.replace(path)


class Builder:
    def __init__(self, output, cli, plugin, target, preview_only, api_key):
        self.output = output
        self.cli = cli
        self.plugin = plugin
        self.target = target
        self.preview_only = preview_only
        self.api_key = api_key
        self.pool = ThreadPoolExecutor(max_workers=1)
        self.lock = threading.Lock()
        self.output.mkdir(mode=0o700, parents=True, exist_ok=True)
        self.blocker = self.output / "install-needs-review"

    def recover(self):
        for job in sorted(self.output.iterdir()):
            if not job.is_dir() or not (job / "walk.jsonl").is_file():
                continue
            state = self.status(job)
            if state == "f":
                atomic_write(self.blocker, job.name.encode())
                self.set_status(job, "e")
        for job in sorted(self.output.iterdir()):
            if job.is_dir() and (job / "walk.jsonl").is_file() and self.status(job) in (None, "b"):
                self.set_status(job, "b")
                self.pool.submit(self.run, job)

    @staticmethod
    def set_status(job, state):
        atomic_write(job / "status", state.encode())
        print(f"Walk {job.name}: {state}", flush=True)

    @staticmethod
    def status(job):
        path = job / "status"
        return path.read_text() if path.exists() else None

    def accept(self, ident, raw):
        walk = split_walks(raw)
        if len(walk) > 1 or json.loads(raw.splitlines()[0]) != {"type": "start"}:
            raise ValueError("Expected one walk")
        job = self.output / ident
        with self.lock:
            job.mkdir(mode=0o700, exist_ok=True)
            path = job / "walk.jsonl"
            if path.exists():
                return 200 if path.read_bytes() == raw else 409
            atomic_write(path, raw)
            self.set_status(job, "b")
            self.pool.submit(self.run, job)
        return 202

    def run(self, job):
        log = job / "build.log"
        walk = job / "walk.jsonl"
        try:
            try:
                read_route(walk)
            except ValueError as exc:
                if "needs at least two GPS fixes" in str(exc) or "under five meters" in str(exc):
                    self.set_status(job, "n")
                    return
                raise
            copy = make_copy(walk, self.api_key)
            atomic_write(job / "game-copy.json", json.dumps(copy).encode())
            project = job / "project"
            project.mkdir(exist_ok=True)
            for name in ("assets", "project", "plugins"):
                shutil.copytree(GAME / name, project / name, dirs_exist_ok=True)
            descriptor = project / "project.gbsproj"
            shutil.copyfile(GAME / descriptor.name, descriptor)
            build_project(walk, project, copy)
            rom = job / "corner-walk.gbc"
            with log.open("a") as output, tempfile.TemporaryDirectory(prefix="corner-walk-compile-") as scratch:
                subprocess.run(["node", str(self.cli), "make:rom", str(descriptor), str(rom)],
                               check=True, stdout=output, stderr=subprocess.STDOUT, timeout=180,
                               env={**os.environ, "TMPDIR": scratch})
            if self.preview_only or self.blocker.exists():
                self.set_status(job, "r")
                return
            self.set_status(job, "f")
            result = subprocess.run([
                "node", str(ROOT / "relay/install_chromatic.mjs"), str(self.plugin), str(GAME),
                str(rom), json.dumps(self.target), job.name, str(job / "install.jsonl"), "install",
            ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if result.returncode not in (0, 2):
                atomic_write(self.blocker, job.name.encode())
            self.set_status(job, {0: "i", 2: "r"}.get(result.returncode, "e"))
        except Exception as error:
            if self.status(job) == "f":
                atomic_write(self.blocker, job.name.encode())
            with log.open("a") as output:
                output.write(f"{type(error).__name__}: {error}\n")
            self.set_status(job, "e")


class Receiver(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self, status, data=b""):
        self.send_response(status)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        if data:
            self.wfile.write(data)

    def identify(self):
        match = WALK_PATH.fullmatch(self.path)
        if not match:
            self.reply(404)
            return None
        auth = self.headers.get("Authorization", "").encode()
        if not hmac.compare_digest(auth, self.server.authorization):
            self.reply(401)
            return None
        return match[1]

    def do_GET(self):
        if ident := self.identify():
            state = self.server.builder.status(self.server.builder.output / ident)
            self.reply(200, state.encode()) if state else self.reply(404)

    def do_POST(self):
        if ident := self.identify():
            self.connection.settimeout(12)
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if not 0 < length <= LIMIT:
                    return self.reply(413)
                raw = self.rfile.read(length)
                if len(raw) != length:
                    return self.reply(400)
                return self.reply(self.server.builder.accept(ident, raw))
            except (ValueError, OSError, AttributeError, KeyError, TypeError):
                return self.reply(400)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8767)
    for key in ("token", "cli", "plugin", "output", "target"):
        parser.add_argument("--" + key, required=True, type=Path)
    parser.add_argument("--preview-only", action="store_true")
    parser.add_argument("--self-test", type=Path)
    parser.add_argument("--test-walk", type=Path, default=GAME / "sample-corner.jsonl")
    parser.add_argument("--wait-for-port", action="store_true")
    parser.add_argument("--retry-build", help="Rebuild one stored walk that failed before device selection")
    args = parser.parse_args()
    os.umask(0o077)
    key = private_key()
    target = json.loads(args.target.read_text())
    if args.self_test:
        test = Builder(args.self_test.resolve(), args.cli, args.plugin, target, True, key)
        job = test.output / "sample"
        job.mkdir(exist_ok=True)
        atomic_write(job / "walk.jsonl", args.test_walk.read_bytes())
        test.set_status(job, "b")
        test.run(job)
        if test.status(job) != "r":
            raise SystemExit(f"Sample build failed. Check {job / 'build.log'}")
        print("Sol Ultrafast and sample build verified", flush=True)
    builder = Builder(args.output.resolve(), args.cli, args.plugin, target, args.preview_only, key)
    address = ("127.0.0.1", args.port)
    for attempt in range(60 if args.wait_for_port else 1):
        try:
            server = ThreadingHTTPServer(address, Receiver)
            break
        except OSError as exc:
            if not args.wait_for_port or exc.errno != errno.EADDRINUSE:
                raise
            if attempt == 0:
                print("Replacement verified. Waiting for the previous receiver to release the port.", flush=True)
            time.sleep(2)
    else:
        raise SystemExit("The previous receiver still owns the port.")
    with server:
        server.authorization = ("Bearer " + args.token.read_text().strip()).encode()
        server.builder = builder
        builder.recover()
        if args.retry_build:
            if not re.fullmatch(r"[a-f0-9]{32}", args.retry_build):
                raise SystemExit("Invalid saved walk ID.")
            job = builder.output / args.retry_build
            if builder.status(job) != "e" or not (job / "walk.jsonl").is_file() or (job / "install.jsonl").exists():
                raise SystemExit("This walk is not eligible for a pre-install build retry.")
            builder.set_status(job, "b")
            builder.pool.submit(builder.run, job)
        print(f"Corner Walk receiver listening on loopback port {args.port}", flush=True)
        server.serve_forever()


if __name__ == "__main__":
    main()
