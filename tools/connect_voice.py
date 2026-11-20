"""Set the authenticated HTTPS session broker on a USB-connected Corner Walk board."""
import argparse
from pathlib import Path
from urllib.parse import urlsplit
from export_walk import request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--url", required=True, help="HTTPS session endpoint, including /session")
    parser.add_argument("--token-file", type=Path, required=True)
    args = parser.parse_args()
    url = urlsplit(args.url)
    if url.scheme != "https" or not url.hostname or url.username or url.password or url.path != "/session":
        parser.error("Use the HTTPS /session URL of your broker.")
    secret = args.token_file.read_bytes().strip()
    if not (32 <= len(secret) < 64) or not all(chr(c).isalnum() or c in b"-_" for c in secret):
        parser.error("Invalid board token.")
    if args.token_file.stat().st_mode & 0o077:
        parser.error("Restrict the token file to its owner (chmod 600).")
    result = request(args.port, True, status_command=b"VOICE SET\t" + args.url.encode() + b"\t" + secret)
    if result != "STATUS VOICE ready":
        raise SystemExit("The board could not initialize voice.")
    print(result)


if __name__ == "__main__":
    main()
