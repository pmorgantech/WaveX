#!/usr/bin/env python3
"""Capture a screenshot from the running ESP32 UI over serial.

Sends the trigger token to the ESP32's console tty, then harvests the
RLE+base64 dump the firmware prints between BEGIN/END markers and decodes it
to PNG. Debug builds only (WAVEX_ESP_SCREENSHOT_DEBUG).

Designed to coexist with the serial logger: by default the dump is read from
logs/esp32.log, which serial_log.py is already appending to - the logger owns
reading the port, this script only writes the trigger to it. Use --direct
only when no logger is running.

Usage:
  scripts/esp32_screenshot.py                    # logger running (default)
  scripts/esp32_screenshot.py --out shots/a.png
  scripts/esp32_screenshot.py --direct           # no logger; read the port
"""

import argparse
import base64
import os
import re
import struct
import sys
import time
import zlib

TOKEN = b"WAVEX-SCREENSHOT\n"
BEGIN_RE = re.compile(
    rb"=== WAVEX SCREENSHOT BEGIN w=(\d+) h=(\d+) "
    rb"fmt=rgb565 enc=rle\+b64 len=(\d+) ==="
)
END_RE = re.compile(rb"=== WAVEX SCREENSHOT END crc=([0-9a-f]{8}) ===")
ERROR_RE = re.compile(rb"=== WAVEX SCREENSHOT ERROR (\S+) ===")


def write_png(path, w, h, rgb):
    def chunk(t, data):
        c = struct.pack(">I", len(data)) + t + data
        return c + struct.pack(">I", zlib.crc32(t + data) & 0xFFFFFFFF)

    raw = bytearray()
    row = w * 3
    for y in range(h):
        raw.append(0)
        start = y * row
        end = start + row
        raw += rgb[start:end]
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def decode(w, h, rle, out_path):
    rgb = bytearray(w * h * 3)
    px = 0
    i = 0
    n = len(rle)
    total = w * h
    while i + 2 < n and px < total:
        run = rle[i]
        lo = rle[i + 1]
        hi = rle[i + 2]
        i += 3
        p = lo | (hi << 8)
        r = ((p >> 11) & 0x1F) << 3
        g = ((p >> 5) & 0x3F) << 2
        b = (p & 0x1F) << 3
        for _ in range(run):
            if px >= total:
                break
            o = px * 3
            rgb[o] = r
            rgb[o + 1] = g
            rgb[o + 2] = b
            px += 1
    if px != total:
        print(f"warning: decoded {px} of {total} pixels", file=sys.stderr)
    write_png(out_path, w, h, rgb)


def send_trigger(port):
    fd = os.open(port, os.O_WRONLY | os.O_NOCTTY)
    try:
        os.write(fd, TOKEN)
    finally:
        os.close(fd)


def harvest(read_line, timeout_s):
    """Consumes lines until a complete dump is seen; returns (w, h, rle)."""
    deadline = time.time() + timeout_s
    header = None
    b64 = bytearray()
    while time.time() < deadline:
        line = read_line()
        if line is None:
            time.sleep(0.05)
            continue
        err = ERROR_RE.search(line)
        if err:
            name = err.group(1).decode()
            sys.exit(f"firmware reported screenshot error: {name}")
        if header is None:
            m = BEGIN_RE.search(line)
            if m:
                header = (int(m.group(1)), int(m.group(2)), int(m.group(3)))
            continue
        m = END_RE.search(line)
        if m:
            rle = base64.b64decode(bytes(b64), validate=False)
            w, h, rle_len = header
            if len(rle) != rle_len:
                sys.exit(f"length mismatch: got {len(rle)}, want {rle_len}")
            crc = zlib.crc32(rle) & 0xFFFFFFFF
            want = int(m.group(1), 16)
            if crc != want:
                sys.exit(f"crc mismatch: got {crc:08x}, want {want:08x}")
            return w, h, rle
        b64 += line.strip()
    sys.exit(
        "timed out waiting for screenshot dump (is this a debug build, "
        "and is the firmware running?)"
    )


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", default="/dev/ttyACM0", help="ESP32 console tty")
    ap.add_argument(
        "--log",
        default="logs/esp32.log",
        help="logfile the serial logger is appending to",
    )
    ap.add_argument(
        "--direct",
        action="store_true",
        help="read the port directly (only when no logger holds it)",
    )
    ap.add_argument("--out", default=None, help="output PNG path")
    ap.add_argument("--timeout", type=float, default=90.0)
    args = ap.parse_args()

    out = args.out or time.strftime("esp32-screen-%Y%m%d-%H%M%S.png")

    if args.direct:
        import termios

        fd = os.open(args.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        attrs = termios.tcgetattr(fd)
        attrs[0] = attrs[1] = attrs[3] = 0  # raw
        attrs[2] = (attrs[2] | termios.CLOCAL | termios.CREAD) & ~termios.HUPCL
        attrs[4] = attrs[5] = termios.B115200
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        os.write(fd, TOKEN)
        buf = bytearray()

        def read_line():
            nonlocal buf
            try:
                chunk = os.read(fd, 4096)
                buf += chunk
            except BlockingIOError:
                pass
            if b"\n" in buf:
                line, _, buf2 = bytes(buf).partition(b"\n")
                buf = bytearray(buf2)
                return line
            return None

        w, h, rle = harvest(read_line, args.timeout)
        os.close(fd)
    else:
        if not os.path.exists(args.log):
            sys.exit(
                f"{args.log} not found - start the logger (make logs-start) "
                "or use --direct"
            )
        f = open(args.log, "rb")
        f.seek(0, os.SEEK_END)  # only accept output newer than the trigger
        send_trigger(args.port)

        def read_line():
            line = f.readline()
            return line.rstrip(b"\r\n") if line else None

        w, h, rle = harvest(read_line, args.timeout)
        f.close()

    decode(w, h, rle, out)
    print(f"wrote {out} ({w}x{h})")


if __name__ == "__main__":
    main()
