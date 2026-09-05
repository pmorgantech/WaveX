#!/usr/bin/env python3
"""Evaluate and optionally record the Daisy callback-headroom gate.

The input is a serial capture from a persistent QSPI, -O2 profiling build.
It must contain the raw DWT fields emitted for the ``audio_callback`` zone.

Examples:
    scripts/callback_performance.py logs/daisy.log \
        --scenario "8 voices, 24 dB SVF + drive" --voices 8 \
        --features-remaining yes --underruns 0
    scripts/callback_performance.py logs/daisy.log \
        --scenario "8 voices, 24 dB SVF + drive" --voices 8 \
        --features-remaining yes --underruns 0 --record "Phase 2 checkpoint"
"""

import argparse
import datetime as dt
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
REPORT = ROOT / "docs" / "callback-performance-log.md"

PROFILE_RE = re.compile(
    r"audio_callback:\s+calls=(?P<calls>\d+)\s+"
    r"avg_cycles=(?P<avg>\d+)\s+max_cycles=(?P<max>\d+)"
)
CAPTURE_HELP = "Daisy serial capture"
COMMIT_HELP = "override the commit column (backfill)"


def git(*args):
    return subprocess.run(
        ["git", *args], cwd=ROOT, capture_output=True, text=True, check=True
    ).stdout.strip()


def describe_tree():
    commit = git("rev-parse", "--short", "HEAD")
    dirty = git("status", "--porcelain", "--untracked-files=no") != ""
    return commit + ("+" if dirty else "")


def parse_capture(path):
    total_calls = 0
    weighted_cycles = 0
    max_cycles = 0
    intervals = 0
    with path.open(errors="replace") as capture:
        for line in capture:
            match = PROFILE_RE.search(line)
            if not match:
                continue
            calls = int(match.group("calls"))
            if calls == 0:
                continue
            avg_cycles = int(match.group("avg"))
            total_calls += calls
            weighted_cycles += avg_cycles * calls
            max_cycles = max(max_cycles, int(match.group("max")))
            intervals += 1
    if total_calls == 0:
        sys.exit(
            f"{path}: no raw audio_callback DWT records found; build with "
            "WAVEX_PROFILING_ENABLED=ON and capture the profiling output"
        )
    return (
        intervals,
        total_calls,
        round(weighted_cycles / total_calls),
        max_cycles,
    )


def classify(max_pct, features_remaining):
    if max_pct < 60.0:
        return "COMFORTABLE", True
    if max_pct < 70.0:
        return "STAY", True
    if max_pct < 80.0:
        return "REVIEW", False
    if features_remaining:
        return "UPGRADE", False
    return "HOLD", False


def clean_cell(value):
    return " ".join(value.replace("|", "/").split())


def format_row(args, stats, decision):
    intervals, calls, avg_cycles, max_cycles = stats
    budget_cycles = args.core_hz * args.block_size / args.sample_rate
    avg_pct = 100.0 * avg_cycles / budget_cycles
    max_pct = 100.0 * max_cycles / budget_cycles
    duration_s = calls * args.block_size / args.sample_rate
    headroom_pct = 100.0 - max_pct
    date = args.date or dt.date.today().isoformat()
    commit = args.commit or describe_tree()
    remaining = "yes" if args.features_remaining == "yes" else "no"
    return (
        f"| {date} | {commit} | {clean_cell(args.scenario)} | {args.voices} | "
        f"{args.sample_rate}/{args.block_size} | "
        f"{args.core_hz / 1_000_000:g} MHz | "
        f"QSPI `-O2` + DWT | {duration_s:.1f}s ({intervals} windows) | "
        f"{avg_cycles} ({avg_pct:.1f}%) | {max_cycles} ({max_pct:.4f}%) | "
        f"{headroom_pct:.4f}% | {args.underruns} | {remaining} | {decision} | "
        f"{clean_cell(args.record or '')} |\n"
    )


def append_row(row):
    text = REPORT.read_text()
    stripped = text.rstrip("\n") + "\n"
    if not re.search(r"\n\|[^\n]*\|\n$", stripped):
        sys.exit(f"{REPORT} must end with the performance table")
    REPORT.write_text(stripped + row)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("capture", type=pathlib.Path, help=CAPTURE_HELP)
    parser.add_argument(
        "--scenario", required=True, help="worst-case workload exercised"
    )
    parser.add_argument(
        "--voices",
        required=True,
        type=int,
        help="simultaneously active voices",
    )
    parser.add_argument(
        "--features-remaining",
        required=True,
        choices=("yes", "no"),
        help="whether planned callback features remain after this checkpoint",
    )
    parser.add_argument("--sample-rate", type=int, default=48000)
    parser.add_argument("--block-size", type=int, default=48)
    parser.add_argument("--core-hz", type=int, default=480_000_000)
    parser.add_argument(
        "--underruns",
        required=True,
        type=int,
        help="underruns observed during the capture",
    )
    parser.add_argument(
        "--record", metavar="NOTE", help="append the result to the project log"
    )
    parser.add_argument("--commit", help=COMMIT_HELP)
    parser.add_argument("--date", help="override the date column (backfill)")
    args = parser.parse_args()

    for name in ("voices", "sample_rate", "block_size", "core_hz"):
        if getattr(args, name) <= 0:
            parser.error(f"--{name.replace('_', '-')} must be positive")
    if args.underruns < 0:
        parser.error("--underruns cannot be negative")
    if not args.capture.is_file():
        parser.error(f"capture does not exist: {args.capture}")

    stats = parse_capture(args.capture)
    duration_s = stats[1] * args.block_size / args.sample_rate
    if duration_s < 600.0:
        parser.error(
            f"capture covers only {duration_s:.1f}s of profiled callbacks; "
            "the gate requires at least 600s"
        )
    budget_cycles = args.core_hz * args.block_size / args.sample_rate
    max_pct = 100.0 * stats[3] / budget_cycles
    decision, passes = classify(max_pct, args.features_remaining == "yes")
    if args.underruns:
        decision += "+UNDERRUN"
        passes = False
    row = format_row(args, stats, decision)
    sys.stdout.write(row)
    if args.record is not None:
        append_row(row)
        print(f"recorded in {REPORT.relative_to(ROOT)}")

    if not passes:
        if args.underruns:
            print(
                "gate blocked: callback run reported underruns",
                file=sys.stderr,
            )
        elif decision == "REVIEW":
            message = "gate blocked: profile/optimize or reduce scope"
            print(
                f"{message}, then re-measure",
                file=sys.stderr,
            )
        elif decision == "UPGRADE":
            print(
                "gate blocked: activate the backend chip-upgrade path",
                file=sys.stderr,
            )
        else:
            print(
                "gate blocked: reduce load or explicitly close further "
                "callback scope",
                file=sys.stderr,
            )
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
