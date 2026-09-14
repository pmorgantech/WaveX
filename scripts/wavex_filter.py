#!/usr/bin/env python3
"""Set the Daisy's per-voice filter slope/drive at runtime, for listening.

Sends "WAVEX-FILTER ..." to the Daisy's console port (debug builds only:
WAVEX_DEBUG_HARNESS_ENABLED) and prints the board's confirmation line.
Which filter topology renders a voice (SVF or ladder) is the Instrument's
own setting on the Filter page, not a console switch; this shapes whichever
one each Instrument selects.

  wavex_filter.py '?'                # report the current selection
  wavex_filter.py 24                 # 24 dB/oct, drive as last set
  wavex_filter.py 12 60              # 12 dB/oct, drive 60%

The selection is not a wire parameter and is not saved: it lives until the
next reboot. Same transport rules as wavex_log.py.
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wavex_log import BOARDS, send, tail_confirmation  # noqa: E402


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument(
        "slope",
        help="12 | 24 | ?",
    )
    ap.add_argument(
        "drive",
        nargs="?",
        help="0-100 percent",
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

    if args.slope == "?":
        cmd = "WAVEX-FILTER ?"
    else:
        if args.slope not in ("12", "24"):
            ap.error("slope must be 12 or 24")
        drive_ok = args.drive is None or (
            args.drive.isdigit() and int(args.drive) <= 100
        )
        if not drive_ok:
            ap.error("drive must be 0-100")
        parts = ["WAVEX-FILTER", args.slope]
        if args.drive:
            parts.append(args.drive)
        cmd = " ".join(parts)

    port = send("daisy", cmd)
    print(f"sent {cmd!r} to {port}")
    if not args.no_confirm:
        tail_confirmation(
            BOARDS["daisy"]["log"], args.timeout, 1, marker=b"WAVEX-FILTER"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
