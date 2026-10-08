#!/usr/bin/env python3
"""Client for the DK64NX_REMOTE test harness (switch/remote_switch.cpp), port 28280.

Buttons are the Switch ones (A B X Y L R ZL ZR PLUS MINUS UP DOWN LEFT RIGHT LS RS), held for the given
milliseconds; the sticks take x y in [-1, 1], y up.

Examples
--------
    dk64nx_remote.py --host 172.22.22.99 status
    dk64nx_remote.py press LS+RS 200        # open the NX Options menu
    dk64nx_remote.py press A 200
    dk64nx_remote.py stick 0 1 1500
    dk64nx_remote.py rstick 1 0 300
    dk64nx_remote.py shot capture.png
    dk64nx_remote.py peek 8076A0A8 1
    dk64nx_remote.py exp
"""
from __future__ import annotations

import argparse
import socket
import struct
import sys
import zlib
from pathlib import Path

PORT = 28280


def read_line(sock: socket.socket) -> str:
    """Read one newline-terminated reply line."""
    data = bytearray()
    while not data.endswith(b"\n"):
        chunk = sock.recv(1)
        if not chunk:
            raise ConnectionError("connection closed")
        data += chunk
    return data.decode().rstrip("\n")


def read_exact(sock: socket.socket, size: int) -> bytes:
    """Read exactly ``size`` bytes."""
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(min(1 << 20, size - len(data)))
        if not chunk:
            raise ConnectionError("connection closed")
        data += chunk
    return bytes(data)


def write_png(path: Path, width: int, height: int, rgba_bottom_up: bytes) -> None:
    """Write RGBA pixels read with glReadPixels (bottom row first) as an RGB PNG."""
    stride = width * 4
    rows = []
    for y in range(height - 1, -1, -1):
        row = rgba_bottom_up[y * stride:(y + 1) * stride]
        rgb = bytearray(width * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = row[0::4], row[1::4], row[2::4]
        rows.append(b"\x00" + bytes(rgb))

    def chunk(tag: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body))

    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header)
                     + chunk(b"IDAT", zlib.compress(b"".join(rows), 6)) + chunk(b"IEND", b""))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="172.22.22.99")
    parser.add_argument("command", nargs="+", help="press|stick|rstick|shot|peek|status|cpu|exp and its arguments")
    args = parser.parse_args()

    with socket.create_connection((args.host, PORT), timeout=10) as sock:
        if args.command[0] == "shot":
            out = Path(args.command[1] if len(args.command) > 1 else "shot.png")
            sock.sendall(b"shot\n")
            header = read_line(sock)
            if not header.startswith("OK"):
                print(header, file=sys.stderr)
                return 1
            _, width, height, size = header.split()
            write_png(out, int(width), int(height), read_exact(sock, int(size)))
            print(f"saved {out} ({width}x{height})")
            return 0
        sock.sendall((" ".join(args.command) + "\n").encode())
        reply = read_line(sock)
        if args.command[0] == "exp" and reply == "OK":
            while (line := read_line(sock)) != "END":
                print(line)
            return 0
        print(reply)
        return 0 if reply.startswith("OK") else 1


if __name__ == "__main__":
    sys.exit(main())
