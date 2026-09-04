#!/usr/bin/env python3
"""Tune WaveX debug logging at runtime - no reflash, no rebuild.

Sends "WAVEX-LOG <MODULE|*|?> [LEVEL]" to a board's console port and, when
the serial logger is running, tails its logfile for the confirmation line.
The command grammar is implemented by ApplyLevelCommand in
firmware/shared/config/logging_config.h and is identical on both boards;
the ESP32 additionally accepts its ESP_LOG tag names (e.g. UI_NAVIGATOR),
which map onto IDF's per-tag esp_log_level_set.

Levels: OFF ERROR WARN INFO DEBUG TRACE (or 0-5).

Usage:
  wavex_log.py --list                      # module table from logging_config.h
  wavex_log.py daisy INTER_MCU_LINK DEBUG  # deep-dive one Daisy subsystem
  wavex_log.py daisy '*' WARN              # quiet everything on the Daisy
  wavex_log.py daisy '?'                   # print current Daisy levels
  wavex_log.py esp32 UI_NAVIGATOR DEBUG    # per-tag on the ESP32
  wavex_log.py esp32 STORAGE TRACE --no-confirm

Writes the trigger to the port without claiming it for reading, so it
coexists with serial_log.py the same way daisy_dfu_trigger.py does. The
confirmation tail reads logs/<board>.log (start it with `make logs-start`);
--no-confirm skips the wait when no logger is running.
"""

import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from serial_ports import DAISY_CDC, ESP32_UART, find_tty  # noqa: E402

LEVELS = ("OFF", "ERROR", "WARN", "INFO", "DEBUG", "TRACE")
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGGING_CONFIG = os.path.join(
    REPO_ROOT, "firmware", "shared", "config", "logging_config.h"
)

BOARDS = {
    "daisy": {"vidpid": DAISY_CDC, "log": "logs/daisy.log"},
    "esp32": {"vidpid": ESP32_UART, "log": "logs/esp32.log"},
}


def parse_modules():
    """Module names from the X-macro table - the single source of truth."""
    try:
        with open(LOGGING_CONFIG) as fh:
            text = fh.read()
    except OSError as exc:
        sys.exit(f"cannot read {LOGGING_CONFIG}: {exc}")
    block = re.search(
        r"#define WAVEX_LOG_MODULE_LIST\(X\)(.*?)\n\n",
        text,
        re.S,
    )
    if not block:
        sys.exit("WAVEX_LOG_MODULE_LIST not found - logging_config.h moved?")
    return re.findall(r"X\((\w+),", block.group(1))


def send(board, command):
    port = find_tty(*BOARDS[board]["vidpid"])
    if port is None:
        vid, pid = BOARDS[board]["vidpid"]
        sys.exit(f"no {board} on USB (looked for {vid}:{pid})")
    fd = os.open(port, os.O_WRONLY | os.O_NOCTTY)
    try:
        os.write(fd, (command + "\n").encode())
    finally:
        os.close(fd)
    return port


def tail_confirmation(logfile, timeout_s, want_lines, marker=b"WAVEX-LOG"):
    """Print reply lines containing `marker` appended to the logger's file."""
    try:
        fh = open(logfile, "rb")
    except OSError:
        print(f"(no {logfile} - is the logger running? skipping confirm)")
        return
    fh.seek(0, os.SEEK_END)
    deadline = time.time() + timeout_s
    seen = 0
    while time.time() < deadline and seen < want_lines:
        line = fh.readline()
        if not line:
            time.sleep(0.05)
            continue
        if marker in line:
            print(line.decode(errors="replace").rstrip())
            seen += 1
    fh.close()
    if seen == 0:
        print(
            "(no confirmation seen - command may still have applied; "
            "check the board's log)"
        )


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument(
        "--list",
        action="store_true",
        help="print the shared module table",
    )
    ap.add_argument(
        "board",
        nargs="?",
        choices=sorted(BOARDS),
        help="target board",
    )
    ap.add_argument(
        "module",
        nargs="?",
        help="module name, ESP32 tag, '*' or '?'",
    )
    ap.add_argument(
        "level",
        nargs="?",
        help="OFF|ERROR|WARN|INFO|DEBUG|TRACE|0-5",
    )
    ap.add_argument(
        "--no-confirm",
        action="store_true",
        help="don't tail the logfile",
    )
    ap.add_argument(
        "--timeout",
        type=float,
        default=2.0,
        help="confirmation wait (s)",
    )
    args = ap.parse_args()

    if args.list:
        print("Shared log modules (firmware/shared/config/logging_config.h):")
        for name in parse_modules():
            print(f"  {name}")
        print(f"Levels: {' '.join(LEVELS)} (or 0-5)")
        print(
            "The ESP32 also accepts its ESP_LOG tag names "
            "(grep TAG in firmware/esp32).",
        )
        return 0

    if not args.board or not args.module:
        ap.error("need a board and a module (or --list)")
    query = args.module == "?"
    if not query:
        if not args.level:
            ap.error("need a level (or use '?' to query)")
        if args.level.upper() not in LEVELS and args.level not in "012345":
            lv = " ".join(LEVELS)
            ap.error(f"bad level {args.level!r}; use {lv} or 0-5")

    if query:
        cmd = f"WAVEX-LOG {args.module}"
    else:
        cmd = f"WAVEX-LOG {args.module} {args.level}"
    # The query spelling on the wire is "WAVEX-LOG ?".
    port = send(args.board, cmd)
    print(f"sent {cmd!r} to {port}")

    if not args.no_confirm:
        want = len(parse_modules()) if query else 1
        tail_confirmation(BOARDS[args.board]["log"], args.timeout, want)
    return 0


if __name__ == "__main__":
    sys.exit(main())
