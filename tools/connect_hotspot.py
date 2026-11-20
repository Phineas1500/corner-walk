"""Join an iPhone hotspot from the CoreS3 logger over USB on macOS."""

import argparse
import ipaddress
import subprocess
import time

from export_walk import request


def prompt(message, hidden=False):
    # Only the prompt is an argument. The answer travels in a private pipe.
    modifier = " with hidden answer" if hidden else ""
    script = (
        'on run argv\nactivate\n'
        'set d to display dialog (item 1 of argv) with title "CoreS3 hotspot setup" '
        'default answer "" buttons {"Cancel", "Continue"} default button "Continue" '
        f'cancel button "Cancel"{modifier}\nreturn text returned of d\nend run'
    )
    result = subprocess.run(
        ["/usr/bin/osascript", "-e", script, message], capture_output=True,
    )
    if result.returncode:
        raise SystemExit("Hotspot setup dialog closed before completion.")
    return result.stdout.removesuffix(b"\n")


def nearby_networks(port, passive=False):
    scan = b"WIFI PASSIVE" if passive else b"WIFI SCAN"
    if request(port, True, status_command=scan) != "STATUS WIFI scanning":
        return []
    prefix = "STATUS WIFI networks"
    for _ in range(24):
        time.sleep(0.5)
        reply = request(port, True, status_command=b"WIFI LIST")
        if reply.startswith(prefix):
            names = []
            for value in reply[len(prefix):].split():
                try:
                    name = bytes.fromhex(value).decode()
                except (ValueError, UnicodeError):
                    continue
                if name.isprintable() and name not in names:
                    names.append(name)
            return names
        if reply != "STATUS WIFI scanning":
            break
    return []


def choose_hotspot(port):
    script = (
        'on run argv\nactivate\nset s to choose from list argv with title "CoreS3 hotspot setup" '
        'with prompt "Choose your iPhone from the networks the CoreS3 can see. '
        'If it is missing, enable Allow Others to Join and Maximize Compatibility on the iPhone, '
        'leave Personal Hotspot open, then scan again."\n'
        'if s is false then return "0"\n'
        'repeat with i from 1 to count argv\n'
        'if item i of argv is item 1 of s then return i\nend repeat\nend run'
    )
    passive = True
    while True:
        names = nearby_networks(port, passive=passive)
        passive = not passive
        choices = names + ["Scan again", "Enter hotspot name manually…"]
        result = subprocess.run(["/usr/bin/osascript", "-e", script, *choices], capture_output=True)
        if result.returncode or not result.stdout.strip().isdigit() or int(result.stdout) == 0:
            raise SystemExit("Hotspot selection closed.")
        index = int(result.stdout) - 1
        if index < len(names):
            return names[index].encode()
        if index == len(names) + 1:
            return prompt("Enter the hotspot name exactly as shown on your iPhone.")


def connection(port, ssid):
    reply = request(port, True, status_command=b"WIFI CHECK\t" + ssid)
    prefix = "STATUS WIFI connected "
    if reply.startswith(prefix):
        return ipaddress.IPv4Address(reply[len(prefix):])
    if reply != "STATUS WIFI waiting":
        raise RuntimeError("Unexpected response to Wi-Fi check.")
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="CoreS3 USB serial device")
    port = parser.parse_args().port
    if " wifi=" not in request(port, True):
        raise SystemExit("The selected device needs the Wi-Fi version of the walk logger.")
    ssid = choose_hotspot(port)
    if not 1 <= len(ssid) <= 32 or any(c in ssid for c in (b"\t", b"\r", b"\n", b"\0")):
        raise SystemExit("Invalid hotspot name. Use a name between 1 and 32 bytes.")
    address = connection(port, ssid)
    if not address:
        password = prompt(
            "Enter the iPhone Wi-Fi Password for the CoreS3. It will be sent over USB "
            "and saved on the board for the next walk.", hidden=True,
        )
        if not 8 <= len(password) <= 63 or any(c < 32 or c > 126 for c in password):
            raise SystemExit("Use the 8–63 character ASCII password from Personal Hotspot.")
        reply = request(port, True, status_command=b"WIFI JOIN\t" + ssid + b"\t" + password)
        del password
        if reply != "STATUS WIFI joining":
            raise SystemExit("The board could not start joining the hotspot.")
        for _ in range(30):
            time.sleep(1)
            address = connection(port, ssid)
            if address:
                break
    if not address:
        status = request(port, True)
        reason = status.partition("last_wifi_error=")[2].partition(" ")[0]
        if reason == "201":
            raise SystemExit("The CoreS3 could no longer find the selected 2.4 GHz hotspot.")
        if reason in {"15", "202", "204"}:
            raise SystemExit("The CoreS3 found the hotspot but Wi-Fi authentication did not complete. Check the password.")
        raise SystemExit(f"The CoreS3 did not receive an address. Wi-Fi disconnect reason {reason or 'unknown'}.")
    print(f"CoreS3 joined the selected hotspot and received IPv4 address {address}.")


if __name__ == "__main__":
    main()
