"""Export CoreS3 GPS walks to walks/*.jsonl over USB on macOS."""

import argparse
import glob
import json
import os
import re
import select
import tempfile
import termios
import time
import tty
from pathlib import Path


def request(port, status_only, *, status_command=b"STATUS"):
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    previous = termios.tcgetattr(fd)
    try:
        tty.setraw(fd)
        settings = termios.tcgetattr(fd)
        settings[4] = settings[5] = termios.B115200
        termios.tcsetattr(fd, termios.TCSANOW, settings)
        termios.tcflush(fd, termios.TCIFLUSH)
        os.write(fd, status_command + b"\n" if status_only else b"DUMP\n")
        buf = bytearray()
        size = None
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline and select.select([fd], [], [], 6)[0]:
            try:
                chunk = os.read(fd, 65536)
            except BlockingIOError:
                continue
            if not chunk:
                break
            buf.extend(chunk)
            if status_only:
                match = re.search(rb"(?:^|\n)(STATUS [^\n]+)\n", buf)
                if match:
                    return match[1].strip().decode()
            elif size is None:
                if b"ERROR " in buf:
                    raise ValueError("Board reported a walk-log read error")
                match = re.search(rb"(?:^|\n)BEGIN (\d+)\r?\n", buf)
                if match:
                    size = int(match[1])
                    del buf[:match.end()]
            if size is not None and b"\n" in buf[size:]:
                if buf[size:].splitlines()[0] != b"END":
                    raise ValueError("Invalid download terminator")
                return bytes(buf[:size])
        raise TimeoutError("Incomplete USB response. Check the port and logger screen.")
    finally:
        try:
            termios.tcsetattr(fd, termios.TCSANOW, previous)
        finally:
            os.close(fd)


def split_walks(data):
    walks = {}
    session = None
    name = None
    offset = 0
    for line in data.splitlines(keepends=True):
        if not line.endswith(b"\n"):
            raise ValueError("Incomplete line in the flash journal")
        event = json.loads(line)
        if event["type"] == "start":
            session, name = offset, None
        elif event["type"] in {"fix", "stop", "said", "chat", "scene"} and session is not None:
            if name is None:
                stamp = event.get("time")
                if event["type"] != "fix":
                    # Stable on subsequent exports, including when the first fix arrives later.
                    name = f"walk-notes-{session}.jsonl"
                elif not isinstance(stamp, str) or not re.fullmatch(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ", stamp):
                    raise ValueError("Invalid GPS timestamp in journal")
                else:
                    name = f"walk-{stamp.replace(':', '')}-{session}.jsonl"
                walks[name] = bytearray()
            walks[name].extend(line)
        else:
            raise ValueError("Unexpected event in the GPS journal")
        offset += len(line)
    return walks


def read_walks(port):
    # Live diagnostics share the USB console. A log line can interrupt a dump
    # while the microphone or camera is active. Only publish a complete snapshot.
    for attempt in range(3):
        try:
            return split_walks(request(port, False))
        except (TimeoutError, ValueError):
            if attempt == 2:
                raise
            time.sleep(0.3)


def save_walk(path, data):
    old = path.read_bytes() if path.exists() else b""
    if old.startswith(data):
        return
    if not data.startswith(old):
        raise ValueError(f"Existing walk differs from board data: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as output:
        temp = Path(output.name)
        try:
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
            os.replace(temp, path)
        finally:
            temp.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="Board serial port (auto-detected if only one is connected)")
    parser.add_argument("--status", action="store_true", help="Read the logger status")
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[1] / "walks")
    args = parser.parse_args()
    ports = [args.port] if args.port else glob.glob("/dev/cu.usbmodem*")
    if len(ports) != 1:
        parser.error("Connect the CoreS3 or select its device with --port")
    if args.status:
        print(request(ports[0], True))
        return
    walks = read_walks(ports[0])
    for name, data in walks.items():
        path = args.output / name
        save_walk(path, data)
        events = [json.loads(line)["type"] for line in data.splitlines()]
        print(f"{path}: {events.count('fix')} GPS points, {events.count('said')} spoken notes, "
              f"{events.count('chat')} chat captions, {events.count('scene')} camera observations"
              + (f", {events.count('stop')} indoor stops" if "stop" in events else ""))
    if not walks:
        print("Journal read successfully. No GPS points recorded yet.")


if __name__ == "__main__":
    main()
