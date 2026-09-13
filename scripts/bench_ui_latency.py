#!/usr/bin/env python3
"""Measure sequencer touch events through confirmed LVGL refresh completion.

Requires a debug ESP image with the UI latency profiling switch enabled in
hardware_config.h, both boards and active serial loggers. Uses the current
pattern, toggles Track 1 step 2 an even number of times, and leaves playback
running. Does not load samples or change the UART rate. Synthetic touch omits
GT911 sensing; refresh completion is not a physical panel visibility timestamp.
"""

import argparse
import json
import math
import statistics
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests/hil"))
from wavex_target import Daisy, Esp32  # noqa: E402


def stats(values):
    ordered = sorted(values)
    return {
        "n": len(values),
        "mean_us": statistics.mean(values),
        "median_us": statistics.median(values),
        "p95_us": ordered[math.ceil(0.95 * len(ordered)) - 1],
        "max_us": max(values),
        "min_us": min(values),
    }


def measure(esp, x, y, mask):
    previous = int(esp.page("LATENCY")["id"])
    esp.tap(x, y)
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        trace = {key: int(value) for key, value in esp.page("LATENCY").items()}
        if trace["id"] > previous and trace["frame"] and trace["up"]:
            break
    else:
        raise RuntimeError(f"Incomplete trace: {trace}")
    assert trace["down"] <= trace["sent"] <= trace["read"], trace
    assert trace["read"] <= trace["frame"], trace
    assert trace["row"] == 0 and trace["col"] == 1, trace
    esp.wait_state(seqready=1, seqbits=mask)
    return {
        "trace": trace,
        "hold_us": trace["up"] - trace["down"],
        "press_to_send_us": trace["sent"] - trace["down"],
        "send_to_read_us": trace["read"] - trace["sent"],
        "read_to_frame_us": trace["frame"] - trace["read"],
        "press_to_frame_us": trace["frame"] - trace["down"],
        "release_to_frame_us": trace["frame"] - trace["up"],
    }


def run(daisy, esp, count, steady_seconds, capture):
    assert daisy.probe() and esp.probe(), "Both consoles must be live"
    esp.open_menu("Sequencer")
    esp.wait_state(seqready=1)
    esp.page("FOCUS", 1, 2)
    state = esp.wait_state(seqready=1, seqtrack=1, seqpage=1)
    assert int(state["seqlen"]) >= 2, "Pattern needs at least two steps"
    # Check profiling support before changing the pattern or transport.
    esp.page("LATENCY")
    xy = esp.page("CELL", 1, 2)
    x, y = int(xy["x"]), int(xy["y"])
    for playing in (False, True):
        if bool(int(esp.state()["seqplaying"])) != playing:
            esp.softkey("Play" if playing else "Stop")
        esp.wait_state(seqready=1, seqplaying=int(playing))
        if not playing:
            daisy.wait_state(voices=0, timeout=2)
        name = "playback" if playing else "idle"
        time.sleep(2)
        begin = (ROOT / "logs/esp32.log").stat().st_size
        deadline = time.monotonic() + steady_seconds
        while time.monotonic() < deadline:
            daisy.wait_state(underruns=0, dropped=0)
            time.sleep(1)
        capture(begin, name + "-steady")
        begin = (ROOT / "logs/esp32.log").stat().st_size
        samples = []
        mask = int(esp.state()["seqbits"])
        for index in range(count):
            mask ^= 2
            samples.append(measure(esp, x, y, mask))
            if (index + 1) % 10 == 0:
                print(f"{name}: {index + 1}/{count}", flush=True)
        capture(begin, name + "-taps")
        phase = {
            "phase": name,
            "samples": samples,
            "summary": {
                key: stats([sample[key] for sample in samples])
                for key in samples[0]
                if key != "trace"
            },
            "daisy": daisy.wait_state(underruns=0, dropped=0),
        }
        print(json.dumps(phase["summary"]), flush=True)
        yield phase


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True, help="JSON file")
    parser.add_argument("--count", type=int, default=50, help="Even tap count")
    parser.add_argument("--steady-seconds", type=int, default=20)
    args = parser.parse_args()
    if args.count < 2 or args.count % 2 or args.steady_seconds < 0:
        parser.error("count must be positive/even; steady seconds nonnegative")
    if args.out.exists():
        parser.error("output already exists")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    record = {"result": "incomplete", "phases": []}

    def capture(start, name):
        with (ROOT / "logs/esp32.log").open("rb") as source:
            source.seek(start)
            logfile = args.out.with_name(args.out.stem + "-" + name + ".log")
            logfile.write_bytes(source.read())

    daisy, esp = Daisy(), Esp32()
    try:
        for phase in run(daisy, esp, args.count, args.steady_seconds, capture):
            record["phases"].append(phase)
            args.out.write_text(json.dumps(record, indent=2) + "\n")
        record["daisy_final"] = daisy.state()
        record["esp32_final"] = esp.state()
        record["result"] = "passed"
    except Exception as error:
        record["error"] = str(error)
        raise
    finally:
        daisy.close()
        esp.close()
        args.out.write_text(json.dumps(record, indent=2) + "\n")


if __name__ == "__main__":
    main()
