#!/usr/bin/env python3
"""Summarise LVGL sysmon lines from a serial capture into statistics.

`CONFIG_LV_USE_PERF_MONITOR_LOG_MODE=y` makes LVGL print one line about
every 300 ms:

  sysmon: 42 FPS (refr_cnt: 13 | redraw_cnt: 13), refr 21ms
  (render 18ms | flush 3ms), CPU 61%

Watching those scroll past tells you very little - the numbers move, and
"it feels slower" is not a measurement. This turns a capture into count,
mean, median, p95, min and max per field, splits it per UI page, and
diffs two captures so an A/B produces a number rather than an impression.

The split of `refr` into `render` and `flush` is the useful part.
`render` is LVGL drawing into the buffer, which a draw-unit accelerator
like the PPA can speed up. `flush` is getting those pixels to the panel,
which it cannot. A page dominated by `flush` will not be helped by any
amount of draw acceleration, and knowing which half dominates is the
whole point of capturing this instead of watching the FPS counter.

Usage:
  sysmon_stats.py logs/esp32.log              # one capture, per page
  sysmon_stats.py logs/off.log logs/on.log    # A/B, B against A
  sysmon_stats.py --page Play logs/esp32.log  # one page only
  sysmon_stats.py --no-pages logs/esp32.log   # whole capture only

Capture with, from the repo root:
  python3 scripts/serial_log.py --vid 1a86 --pid 55d3 --baud 115200 \\
      --out logs/esp32.log
"""

import argparse
import re
import statistics
import sys

# Matches both sysmon variants: LV_SYSMON_PROC_IDLE_AVAILABLE adds
# a "proc N%" field after the total.
LINE_RE = re.compile(
    r"sysmon:\s*"
    r"(?P<fps>\d+)\s*FPS\s*"
    r"\(refr_cnt:\s*(?P<refr_cnt>\d+)\s*\|\s*"
    r"redraw_cnt:\s*(?P<redraw_cnt>\d+)\),\s*"
    r"refr\s*(?P<refr>\d+)ms\s*"
    r"\(render\s*(?P<render>\d+)ms\s*\|\s*"
    r"flush\s*(?P<flush>\d+)ms\),\s*"
    r"CPU\s*(?:\(total\s*)?(?P<cpu>\d+)%"
)

# Page attribution. The firmware already logs navigation, so a capture
# that wandered across screens can still be split per page afterwards.
# This matters: FPS is not comparable between pages, and one average
# over three screens hides exactly the one that is slow.
_NAV = r"UI_NAVIGATOR:\s*(?:Entering page|Returning to page):\s*"
PAGE_RE = re.compile(_NAV + r"(?P<page>.+?)\s*$")
TAB_RE = re.compile(r"UI_TAB_HOST:\s*(?P<page>\S.*?)\s*$")

FIELDS = ["fps", "refr", "render", "flush", "cpu"]
UNITS = {
    "fps": "FPS",
    "refr": "ms",
    "render": "ms",
    "flush": "ms",
    "cpu": "%",
}


def parse(path):
    """Return a list of sample dicts, each tagged with its page."""
    samples = []
    page = "(before first navigation)"
    with open(path, "r", errors="replace") as fh:
        for line in fh:
            hit = PAGE_RE.search(line) or TAB_RE.search(line)
            if hit:
                page = hit.group("page")
                continue
            m = LINE_RE.search(line)
            if m:
                row = {f: int(m.group(f)) for f in FIELDS}
                row["page"] = page
                samples.append(row)
    return samples


def drop_warmup(samples, warmup):
    """Drop the first N samples of each contiguous page run.

    A freshly opened page redraws everything once. Those samples
    describe the transition, not the steady state being compared.
    """
    if warmup <= 0:
        return samples
    kept = []
    seen = 0
    last = object()
    for row in samples:
        if row["page"] != last:
            last = row["page"]
            seen = 0
        seen += 1
        if seen > warmup:
            kept.append(row)
    return kept


def entry_samples(samples):
    """Keep only the first sample of each contiguous page run.

    Page-entry cost is a different measurement from steady state, and it is
    the one that matches "slow to render" meaning "slow to appear". Every
    other view here deliberately discards it as warmup noise, which is how it
    stayed invisible: the discarded sample was the complaint.
    """
    out = []
    last = object()
    for row in samples:
        if row["page"] != last:
            last = row["page"]
            out.append(row)
    return out


def print_entries(label, samples, min_samples):
    order = []
    for r in samples:
        if r["page"] not in order:
            order.append(r["page"])
    print(f"\n{label}: first frame after entering each page")
    head = f"  {'page':<28} {'n':>4} {'med':>7} {'p95':>7}"
    print(head + f" {'max':>7}   render ms")
    for page in order:
        rows = [r["render"] for r in samples if r["page"] == page]
        if len(rows) < min_samples:
            continue
        print(
            f"  {page[:28]:<28} {len(rows):>4} "
            f"{statistics.median(rows):>7.1f} {p95(rows):>7.1f} "
            f"{max(rows):>7.1f}"
        )


def columns(samples):
    return {f: [r[f] for r in samples] for f in FIELDS}


def p95(values):
    """95th percentile, nearest-rank. Sane for small sample counts."""
    ordered = sorted(values)
    idx = int(round(0.95 * len(ordered))) - 1
    return ordered[max(0, min(len(ordered) - 1, idx))]


def print_table(label, samples):
    n = len(samples)
    cols = columns(samples)
    print(f"\n{label}")
    print(f"  {n} samples, ~{n * 0.3:.0f}s at the 300ms sysmon period")
    head = f"  {'field':<8} {'mean':>7} {'med':>7} {'p95':>7}"
    print(head + f" {'min':>7} {'max':>7}")
    for f in FIELDS:
        v = cols[f]
        print(
            f"  {f:<8} {statistics.fmean(v):>7.1f} "
            f"{statistics.median(v):>7.1f} {p95(v):>7.1f} "
            f"{min(v):>7.1f} {max(v):>7.1f}   {UNITS[f]}"
        )


def print_pages(label, samples, min_samples):
    order = []
    for r in samples:
        if r["page"] not in order:
            order.append(r["page"])
    print(f"\n{label}: per page")
    head = f"  {'page':<28} {'n':>4} {'fps':>6} {'render':>8}"
    print(head + f" {'flush':>7} {'r.p95':>7} {'cpu':>6}")
    for page in order:
        rows = [r for r in samples if r["page"] == page]
        if len(rows) < min_samples:
            continue
        c = columns(rows)
        print(
            f"  {page[:28]:<28} {len(rows):>4} "
            f"{statistics.median(c['fps']):>6.1f} "
            f"{statistics.median(c['render']):>8.1f} "
            f"{statistics.median(c['flush']):>7.1f} "
            f"{p95(c['render']):>7.1f} "
            f"{statistics.median(c['cpu']):>6.1f}"
        )
    print(
        f"  (medians; r.p95 is the render tail. Pages with fewer than "
        f"{min_samples} samples are hidden.)"
    )


def print_delta(label_a, a, label_b, b):
    ca, cb = columns(a), columns(b)
    print(f"\nDelta: {label_b} vs {label_a}  (medians)")
    print(f"  {'field':<8} {'A':>8} {'B':>8} {'change':>9}")
    for f in FIELDS:
        ma = statistics.median(ca[f])
        mb = statistics.median(cb[f])
        pct = "n/a" if ma == 0 else f"{(mb - ma) / ma * 100:+.1f}%"
        print(f"  {f:<8} {ma:>8.1f} {mb:>8.1f} {pct:>9}   {UNITS[f]}")

    # Spread decides whether a delta means anything. If the two runs'
    # middle-50% ranges overlap, a moving median is not yet evidence.
    print("\n  Real, or noise? Compare against run-to-run spread:")
    for f in ("fps", "render", "flush"):
        for lbl, c in ((label_a, ca), (label_b, cb)):
            v = sorted(c[f])
            q1 = v[len(v) // 4]
            q3 = v[(3 * len(v)) // 4]
            name = lbl[-26:]
            iqr = f"IQR {q1:>6.1f} .. {q3:<6.1f} {UNITS[f]}"
            print(f"    {f:<7} {name:<26} {iqr}")


def main():
    ap = argparse.ArgumentParser(
        description="Summarise LVGL sysmon perf lines from a capture."
    )
    ap.add_argument("logs", nargs="+", help="capture file(s); 2 = A/B")
    ap.add_argument(
        "--warmup",
        type=int,
        default=2,
        help="drop N samples after each page change (default 2)",
    )
    ap.add_argument("--page", help="only samples on this page (substring)")
    ap.add_argument("--no-pages", action="store_true", help="no page table")
    ap.add_argument(
        "--entry",
        action="store_true",
        help="first frame after each page entry, not steady state",
    )
    ap.add_argument(
        "--min-samples",
        type=int,
        default=4,
        help="hide pages with fewer than N samples (default 4)",
    )
    args = ap.parse_args()

    parsed = []
    for path in args.logs:
        raw = parse(path)
        if args.entry:
            samples = entry_samples(raw)
        else:
            samples = drop_warmup(raw, args.warmup)
        if args.page:
            want = args.page.lower()
            samples = [r for r in samples if want in r["page"].lower()]
        if not samples:
            what = f" on a page matching {args.page!r}" if args.page else ""
            print(
                f"{path}: no sysmon samples{what}. Is "
                f"CONFIG_LV_USE_PERF_MONITOR_LOG_MODE=y in the flashed "
                f"build?",
                file=sys.stderr,
            )
            return 1
        parsed.append((path, samples))
        print_table(path, samples)
        if args.no_pages or args.page:
            pass
        elif args.entry:
            print_entries(path, samples, args.min_samples)
        else:
            print_pages(path, samples, args.min_samples)

    if len(parsed) == 2:
        print_delta(parsed[0][0], parsed[0][1], parsed[1][0], parsed[1][1])
    elif len(parsed) > 2:
        print("\n(Delta is printed only for exactly two captures.)")

    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
