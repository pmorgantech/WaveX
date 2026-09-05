#!/usr/bin/env python3
"""Evaluate and optionally record the Daisy callback-headroom gate.

The input is a serial capture of one scenario from a persistent QSPI, -O2
profiling build. Everything the row records about the target - core clock,
sample rate, block size, storage layout, optimization level, stream
underruns - is read from the capture, not typed in: the firmware's
``profile_config:`` line and ``audio_callback`` DWT windows are the source
of truth, and the script refuses a capture that lacks them or that spans
more than one serial session / boot (one capture file per scenario).

Examples:
    scripts/callback_performance.py logs/daisy.log \
        --scenario "8 voices, 24 dB SVF + drive" --voices 8 \
        --features-remaining yes
    scripts/callback_performance.py logs/daisy.log \
        --scenario "8 voices, 24 dB SVF + drive" --voices 8 \
        --features-remaining yes --record "Phase 2 checkpoint"
"""

import argparse
import dataclasses
import datetime as dt
import enum
import pathlib
import re
import subprocess
import sys
from typing import NamedTuple, Optional

ROOT = pathlib.Path(__file__).resolve().parent.parent
REPORT = ROOT / "docs" / "callback-performance-log.md"
MIN_DURATION_S = 600.0

# What the firmware prints (firmware/daisy/src/main.cpp PrintProfilingStats)
# once per profiling window, and scripts/serial_log.py's session marker.
CONFIG_RE = re.compile(
    r"profile_config:\s+core_hz=(?P<core_hz>\d+)\s+"
    r"sample_rate=(?P<sample_rate>\d+)\s+block_size=(?P<block_size>\d+)\s+"
    r"storage=(?P<storage>\S+)\s+opt=(?P<opt>\S+)"
)
WINDOW_RE = re.compile(
    r"audio_callback:\s+calls=(?P<calls>\d+)\s+"
    r"avg_cycles=(?P<avg>\d+)\s+max_cycles=(?P<max>\d+)"
)
# AudioEngine::CheckAndLogUnderruns(): stream-ring starvation episodes,
# reported at most once a second with the count since the last report.
UNDERRUN_RE = re.compile(
    r"Ring buffer underrun .*\((?P<episodes>\d+) in last ~1s\)",
)
SESSION_RE = re.compile(r"^--- connected: |=== BOOT COMPLETE")

# The log table. The header written to / verified in the report and the
# cells format_row() emits come from this one list, in this order.
COLUMNS = (
    ("Date", "---"),
    ("Commit", "---"),
    ("Scenario", "---"),
    ("Voices", "---:"),
    ("Hz/block", "---:"),
    ("Core", "---:"),
    ("Image", "---"),
    ("Duration", "---:"),
    ("Budget cycles", "---:"),
    ("Average cycles", "---:"),
    ("Maximum cycles", "---:"),
    ("Worst headroom", "---:"),
    ("Stream underruns", "---:"),
    ("Callback features left", "---"),
    ("Decision", "---"),
    ("Note", "---"),
)
HEADER = (
    "| " + " | ".join(title for title, _ in COLUMNS) + " |\n"
    "|" + "|".join(align for _, align in COLUMNS) + "|\n"
)


class Target(NamedTuple):
    """The parameters one capture was measured under."""

    core_hz: int
    sample_rate: int
    block_size: int
    storage: str
    opt: str

    @property
    def budget_cycles(self) -> float:
        return self.core_hz * self.block_size / self.sample_rate

    @property
    def image(self) -> str:
        return f"{self.storage} `{self.opt}`"


class Capture(NamedTuple):
    """One scenario's aggregate over every profiling window in the file."""

    target: Target
    windows: int
    calls: int
    avg_cycles: int
    max_cycles: int
    underruns: int


class Decision(enum.Enum):
    """Band of the worst observed callback (performance_monitoring.md)."""

    COMFORTABLE = (True, None)
    STAY = (True, None)
    REVIEW = (False, "profile/optimize or reduce scope, then re-measure")
    UPGRADE = (False, "activate the backend chip-upgrade path")
    HOLD = (
        False,
        "reduce load or explicitly close further callback scope",
    )

    @property
    def passes(self) -> bool:
        return self.value[0]

    @property
    def blocked_reason(self) -> Optional[str]:
        return self.value[1]

    @classmethod
    def classify(cls, max_pct, features_remaining) -> "Decision":
        if max_pct < 60.0:
            return cls.COMFORTABLE
        if max_pct < 70.0:
            return cls.STAY
        if max_pct < 80.0:
            return cls.REVIEW
        return cls.UPGRADE if features_remaining else cls.HOLD


@dataclasses.dataclass(frozen=True)
class Measurement:
    """A capture plus what the operator asserts about the run."""

    capture: Capture
    scenario: str
    voices: int
    features_remaining: bool
    underruns: int  # from the capture unless backfilled

    @property
    def target(self) -> Target:
        return self.capture.target

    @property
    def duration_s(self) -> float:
        blocks = self.capture.calls
        return blocks * self.target.block_size / self.target.sample_rate

    @property
    def avg_pct(self) -> float:
        return 100.0 * self.capture.avg_cycles / self.target.budget_cycles

    @property
    def max_pct(self) -> float:
        return 100.0 * self.capture.max_cycles / self.target.budget_cycles

    @property
    def headroom_pct(self) -> float:
        return 100.0 - self.max_pct

    @property
    def decision(self) -> Decision:
        return Decision.classify(self.max_pct, self.features_remaining)

    @property
    def blocked_reason(self) -> Optional[str]:
        """Why the gate is blocked, or None when it passes."""
        if self.underruns:
            return "the run reported stream underruns"
        return self.decision.blocked_reason


def git(*args):
    return subprocess.run(
        ["git", *args], cwd=ROOT, capture_output=True, text=True, check=True
    ).stdout.strip()


def describe_tree():
    commit = git("rev-parse", "--short", "HEAD")
    dirty = git("status", "--porcelain", "--untracked-files=no") != ""
    return commit + ("+" if dirty else "")


def parse_capture(path) -> Capture:
    target = None
    windows = 0
    total_calls = 0
    weighted_cycles = 0
    max_cycles = 0
    underruns = 0
    with path.open(errors="replace") as capture:
        for line in capture:
            if windows and SESSION_RE.search(line):
                sys.exit(
                    f"{path}: a second serial session/boot follows the "
                    "profiling windows; capture one scenario per file"
                )
            config = CONFIG_RE.search(line)
            if config:
                seen = Target(
                    int(config.group("core_hz")),
                    int(config.group("sample_rate")),
                    int(config.group("block_size")),
                    config.group("storage"),
                    config.group("opt"),
                )
                if target is None:
                    target = seen
                elif seen != target:
                    sys.exit(
                        f"{path}: profile_config changes mid-capture "
                        f"({target} -> {seen}); capture one scenario per "
                        "file"
                    )
                continue
            underrun = UNDERRUN_RE.search(line)
            if underrun:
                underruns += int(underrun.group("episodes"))
                continue
            window = WINDOW_RE.search(line)
            if not window:
                continue
            calls = int(window.group("calls"))
            if calls == 0:
                continue
            windows += 1
            total_calls += calls
            weighted_cycles += int(window.group("avg")) * calls
            max_cycles = max(max_cycles, int(window.group("max")))
    if total_calls == 0:
        sys.exit(
            f"{path}: no raw audio_callback DWT records found; build with "
            "WAVEX_PROFILING_ENABLED=ON and capture the profiling output"
        )
    if target is None:
        sys.exit(
            f"{path}: no profile_config line; the capture predates the "
            "firmware that reports its target parameters - rebuild and "
            "re-measure"
        )
    return Capture(
        target,
        windows,
        total_calls,
        round(weighted_cycles / total_calls),
        max_cycles,
        underruns,
    )


def clean_cell(value):
    return " ".join(value.replace("|", "/").split())


def format_row(m: Measurement, date, commit, note):
    target = m.target
    cells = (
        date,
        commit,
        clean_cell(m.scenario),
        str(m.voices),
        f"{target.sample_rate}/{target.block_size}",
        f"{target.core_hz / 1_000_000:g} MHz",
        target.image,
        f"{m.duration_s:.1f}s ({m.capture.windows} windows)",
        f"{target.budget_cycles:.0f}",
        f"{m.capture.avg_cycles} ({m.avg_pct:.1f}%)",
        f"{m.capture.max_cycles} ({m.max_pct:.4f}%)",
        f"{m.headroom_pct:.4f}%",
        str(m.underruns),
        "yes" if m.features_remaining else "no",
        m.decision.name,
        clean_cell(note),
    )
    assert len(cells) == len(COLUMNS)
    return "| " + " | ".join(cells) + " |\n"


def append_row(row):
    text = REPORT.read_text()
    if HEADER not in text:
        sys.exit(
            f"{REPORT} does not contain the table header this script "
            "writes; the columns have diverged"
        )
    stripped = text.rstrip("\n") + "\n"
    if not re.search(r"\n\|[^\n]*\|\n$", stripped):
        sys.exit(f"{REPORT} must end with the performance table")
    REPORT.write_text(stripped + row)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument(
        "capture",
        type=pathlib.Path,
        help="Daisy serial capture of one scenario",
    )
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
    parser.add_argument(
        "--record", metavar="NOTE", help="append the result to the project log"
    )
    backfill = parser.add_argument_group(
        "backfill overrides", "normally taken from the tree or the capture"
    )
    backfill.add_argument("--commit", help="override the commit column")
    backfill.add_argument("--date", help="override the date column")
    backfill.add_argument(
        "--underruns",
        type=int,
        help="override the stream-underrun count read from the capture",
    )
    args = parser.parse_args()

    if args.voices <= 0:
        parser.error("--voices must be positive")
    if args.underruns is not None and args.underruns < 0:
        parser.error("--underruns cannot be negative")
    if not args.capture.is_file():
        parser.error(f"capture does not exist: {args.capture}")

    capture = parse_capture(args.capture)
    if capture.target.storage != "qspi":
        parser.error(
            f"capture is from a {capture.target.storage} image; the gate "
            "needs the persistent QSPI build"
        )
    m = Measurement(
        capture,
        args.scenario,
        args.voices,
        args.features_remaining == "yes",
        capture.underruns if args.underruns is None else args.underruns,
    )
    if m.duration_s < MIN_DURATION_S:
        parser.error(
            f"capture covers only {m.duration_s:.1f}s of profiled "
            f"callbacks; the gate requires at least {MIN_DURATION_S:.0f}s"
        )

    row = format_row(
        m,
        args.date or dt.date.today().isoformat(),
        args.commit or describe_tree(),
        args.record or "",
    )
    sys.stdout.write(row)
    if args.record is not None:
        append_row(row)
        print(f"recorded in {REPORT.relative_to(ROOT)}")

    if m.blocked_reason is not None:
        print(f"gate blocked: {m.blocked_reason}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
