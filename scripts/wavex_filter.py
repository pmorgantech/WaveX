#!/usr/bin/env python3
"""Switch the Daisy's per-voice lowpass at runtime, for A/B listening.

Sends "WAVEX-FILTER ..." to the Daisy's console port (debug builds only:
WAVEX_DEBUG_HARNESS_ENABLED) and prints the board's confirmation line.

  wavex_filter.py '?'                # report the current selection
  wavex_filter.py wavex              # first-party TPT SVF, as configured
  wavex_filter.py wavex 24           # ... 24 dB/oct
  wavex_filter.py wavex 12 60        # ... 12 dB/oct, drive 60%
  wavex_filter.py daisysp            # daisysp::Svf (drive as last set)
  wavex_filter.py daisysp 12 30      # slope is ignored by daisysp::Svf

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
        "topology",
        help="wavex | mine | daisysp | dsp | ?",
    )
    ap.add_argument(
        "slope",
        nargs="?",
        help="12 or 24 (WaveX SVF only)",
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

    if args.topology == "?":
        cmd = "WAVEX-FILTER ?"
    else:
        if args.topology not in ("wavex", "mine", "daisysp", "dsp"):
            ap.error(f"bad topology {args.topology!r}")
        if args.slope and args.slope not in ("12", "24"):
            ap.error("slope must be 12 or 24")
        drive_ok = args.drive is None or (
            args.drive.isdigit() and int(args.drive) <= 100
        )
        if not drive_ok:
            ap.error("drive must be 0-100")
        parts = ["WAVEX-FILTER", args.topology]
        if args.slope:
            parts.append(args.slope)
            if args.drive:
                parts.append(args.drive)
        elif args.drive:
            ap.error("give a slope (12|24) before a drive value")
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
