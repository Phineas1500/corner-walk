"""Pair this walk's StickS3 and CoreS3 over USB."""
import argparse
import re
import secrets
from export_walk import request


def identity(port, role):
    reply = request(port, True, status_command=b"SENSOR ID")
    match = re.fullmatch(r"STATUS SENSOR ID " + role + r" ([0-9a-f]{12})", reply)
    if not match:
        raise ValueError(f"Expected {role} sensor firmware on {port}")
    return match.group(1).encode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stick", required=True)
    parser.add_argument("--core", required=True)
    args = parser.parse_args()
    stick, core = identity(args.stick, "stick"), identity(args.core, "core")
    key = secrets.token_hex(16).encode()
    for board, peer in [(args.stick, core), (args.core, stick)]:
        command = b"SENSOR PAIR " + peer + b" " + key
        if request(board, True, status_command=command) != "STATUS sensor paired":
            raise RuntimeError("Pairing failed. Reconnect both boards and rerun pairing.")
    print("Both boards saved the same private ESP-NOW pairing and are restarting.")


if __name__ == "__main__":
    main()
