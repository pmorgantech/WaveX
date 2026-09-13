#!/usr/bin/env python3
"""Compare Play, Instrument and Diagnostics redraws with ESP32 RENDER.

Requires both boards, serial loggers, a loaded Instrument on --track and a
UI profiling image. Stops sequencing, applies that Instrument's current sound
as its undo point, and exercises/reverts small parameter changes. Play touch
coordinates are from the fixed landscape layout, verified by bench screenshots.
Run separately on each candidate; firmware flashing is intentionally separate.
"""

import argparse
import json
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests/hil"))
from test_sequencer_tracks import _transport  # noqa: E402
from wavex_target import Daisy, Esp32  # noqa: E402

STAGES = [
    ("Osc", "oscready", "LEVEL", "osclevel", 100),
    ("Env", "modready", "ATTACK", "attack", 25),
    ("Amp", "editready", "PAN", "instpan", 10),
    ("Filter", "editready", "RES", "instres", 1000),
    ("Mod", "modready", "DEPTH", "moddepth", 1000),
    ("LFO", "lfoready", "RATE", "rate", 100),
]


def action(esp, name):
    if name not in esp.softkeys():
        esp.key("SHIFT")
        esp.wait_state(shift=1)
    esp.softkey(name)
    esp.wait_state(editpending=0, editdirty=0)


def capture(esp, daisy, out, record, name, work):
    log = ROOT / "logs/esp32.log"
    page = esp.state()["page"]
    start = log.stat().st_size
    esp.cmd("RENDER", "RESET")
    work()
    stats = {key: int(value) for key, value in esp.cmd("RENDER").items()}
    stats["mean_pixels"] = stats["pixels"] / max(1, stats["frames"])
    stats["mean_refresh_us"] = stats["time"] / max(1, stats["frames"])
    record["phases"].append({"name": name, **stats})
    daisy.wait_state(underruns=0, dropped=0)
    with log.open("rb") as source:
        source.seek(start)
        logfile = out.with_name(out.stem + "-" + name + ".log")
        logfile.write_bytes(source.read())
    out.write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps({"name": name, **stats}), flush=True)
    if esp.state()["page"] != page:
        raise RuntimeError("page changed during capture: " + name)


def run(esp, daisy, args, record):
    assert esp.probe() and daisy.probe()
    esp.cmd("RENDER")  # Require profiling before changing the session.
    _transport(daisy, False)
    daisy.wait_state(voices=0)
    esp.track(args.track)
    if args.diagnostics_only:
        esp.open_menu("Diagnostics")
        tabs = "ESP32 Daisy Audio Link Storage MIDI Panel".split()
        for tab in tabs:
            time.sleep(1)
            capture(
                esp,
                daisy,
                args.out,
                record,
                "diagnostics-" + tab,
                lambda: time.sleep(args.seconds),
            )
            esp.softkey("Tab >")
        esp.softkey("Freeze")
        time.sleep(1)
        capture(
            esp,
            daisy,
            args.out,
            record,
            "diagnostics-frozen",
            lambda: time.sleep(args.seconds),
        )
        esp.home()
        record["daisy"] = daisy.wait_state(underruns=0, dropped=0)
        record["result"] = "passed"
        return
    surfaces = [("pads", 320, (130, 185)), ("keys", 955, (40, 500))]
    for tab, tab_x, point in surfaces:
        esp.open_menu("Play")
        esp.tap(tab_x, 100)
        esp.wait_state(page="Play")
        time.sleep(1)
        capture(
            esp,
            daisy,
            args.out,
            record,
            "play-" + tab + "-idle",
            lambda: time.sleep(args.seconds),
        )
        for latched in (False, True):
            if latched:
                esp.softkey("Latch")
                esp.wait_state(sk5="Latch*")
            time.sleep(0.5)

            def taps():
                for _ in range(args.count):
                    esp.tap(*point)
                    time.sleep(0.5)

            name = "play-" + tab + ("-latch" if latched else "-momentary")
            capture(esp, daisy, args.out, record, name, taps)
            if latched:
                esp.softkey("Latch*")
                esp.wait_state(sk5="Latch")
        esp.home()
        daisy.wait_state(voices=0)
    esp.open_menu("Instrument")
    initial = esp.wait_state(oscready=1, oscvalid=1, editpending=0)
    if initial["editdirty"] == "1":
        action(esp, "Apply")
    for tab, ready, verb, field, increment in STAGES:
        esp.page("TAB", tab)
        state = esp.wait_state(tab=tab, editpending=0, **{ready: 1})
        original = int(state[field])
        # Use the opposite direction near the upper end of a control's range.
        limit = {"Amp": 1000, "Mod": 32767, "LFO": 50000}.get(tab, 64000)
        changed = original + increment
        if changed > limit:
            changed = original - increment
        time.sleep(1)
        capture(
            esp,
            daisy,
            args.out,
            record,
            "instrument-" + tab + "-idle",
            lambda: time.sleep(args.seconds),
        )

        def edits():
            for index in range(args.count):
                value = changed if index % 2 == 0 else original
                esp.page(verb, value)
                esp.wait_state(editpending=0, **{ready: 1, field: value})
                time.sleep(0.1)

        name = "instrument-" + tab + "-edits"
        capture(esp, daisy, args.out, record, name, edits)
        if esp.state()["editdirty"] == "1":
            action(esp, "Revert")
    esp.home()
    record["daisy"] = daisy.wait_state(underruns=0, dropped=0)
    record["result"] = "passed"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--track", type=int, default=8, help="Track (0..15)")
    parser.add_argument("--count", type=int, default=10, help="Even count")
    parser.add_argument("--seconds", type=int, default=5)
    parser.add_argument(
        "--diagnostics-only",
        action="store_true",
        help="Measure all seven Diagnostics tabs and Freeze instead",
    )
    args = parser.parse_args()
    if args.out.exists():
        parser.error("output already exists")
    if args.count < 2 or args.count % 2 or args.seconds < 1:
        parser.error("positive seconds and even count >=2 required")
    if not 0 <= args.track < 16:
        parser.error("track must be 0..15")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    record = {"result": "incomplete", "track": args.track, "phases": []}
    daisy, esp = Daisy(), Esp32()
    try:
        run(esp, daisy, args, record)
    except Exception as error:
        record["error"] = str(error)
        raise
    finally:
        daisy.close()
        esp.close()
        args.out.write_text(json.dumps(record, indent=2) + "\n")


if __name__ == "__main__":
    main()
