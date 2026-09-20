#!/usr/bin/env python3
"""Exercise isolated board restarts, browsing, sample loading and sequencing.

Run inside the hardware devcontainer with loggers active. Replaces the bench
session and reflashes Daisy; source card files are untouched.
"""

import argparse
import hashlib
import json
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests/hil"))
from test_load_to_track import _load_and_wait  # noqa: E402
from wavex_target import Daisy, Esp32  # noqa: E402


def wait_probe(target):
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        try:
            if target.probe():
                return
        except OSError:
            pass
        time.sleep(0.2)
    raise RuntimeError(f"{target.board} did not return after restart")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--cycles", type=int, default=3)
    args = parser.parse_args()
    if args.cycles < 1:
        parser.error("--cycles must be positive")
    image = args.image.resolve()
    result = {
        "daisy_sha256": hashlib.sha256(image.read_bytes()).hexdigest(),
        "cycles": [],
    }
    stem = ROOT / "logs" / time.strftime("peer-restart-%Y%m%d-%H%M%S")
    with stem.with_suffix(".log").open("w", buffering=1) as log:
        d, e = Daisy(log), Esp32(log)

        def run(*command):
            subprocess.run(
                command,
                cwd=ROOT,
                stdout=log,
                stderr=log,
                check=True,
            )

        def exercise():
            e.home()
            e.track(0)
            e.open_menu("Sample")
            e.page("TAB", "Browse")
            # Entering Browse already starts a listing of the remembered path.
            # Finish it before requesting another uncorrelated legacy page.
            counts = {"/": 10, "/Drums/Kicks": 51, "/Drums/Loops": 28}
            initial = e.wait_state(tab="Browse")
            e.wait_state(entries=counts[initial["dir"]], timeout=15)
            listings = []
            # Finish each paginated fixture before starting another request;
            # the current browse wire format has no request/path correlation.
            for directory, count in (
                ("/Drums/Kicks", 51),
                ("/", 10),
                ("/Drums/Loops", 28),
                ("/", 10),
            ):
                e.page("DIR", directory)
                state = e.wait_state(dir=directory, entries=count, timeout=15)
                listings.append(
                    {"path": directory, "entries": state["entries"]},
                )
            d.reset_samples()
            e.page("DIR", "/Drums/Kicks")
            e.wait_state(dir="/Drums/Kicks", entries=51, timeout=15)
            e.page("SEL", "bassdr01.wav")
            e.wait_state(sel="bassdr01.wav", picker=0, sk2="Load")
            loaded = _load_and_wait(e)
            assert "loaded_onto_Track" in loaded["status"], loaded
            d.note(0, 60)
            d.wait_state(voices=lambda n: int(n) > 0)
            d.note(0, 60, on=False)
            d.wait_state(voices=0)
            e.open_menu("Sequencer")
            e.wait_state(seqready=1)
            assert d.state()["underruns"] == "0"
            return listings

        try:
            for cycle in range(args.cycles):
                wait_probe(d)
                wait_probe(e)
                # Keep the frontend polling while only its peer disappears.
                e.open_menu("Sequencer")
                e.wait_state(seqready=1)
                run(sys.executable, "scripts/daisy_dfu_trigger.py")
                run(
                    "dfu-util",
                    "-d",
                    "0483:df11",
                    "-s",
                    "0x90040000:leave",
                    "-D",
                    str(image),
                )
                wait_probe(d)
                listings = exercise()
                result["cycles"].append(
                    {"cycle": cycle, "peer": "daisy", "listings": listings}
                )
                print(json.dumps(result["cycles"][-1]), flush=True)
                # Reverse direction: Daisy keeps running with a loaded Track.
                run("make", "esp32-reset")
                wait_probe(e)
                listings = exercise()
                result["cycles"].append(
                    {"cycle": cycle, "peer": "esp32", "listings": listings}
                )
                print(json.dumps(result["cycles"][-1]), flush=True)
            result["result"] = "passed"
        except BaseException as error:
            result["result"] = "failed"
            result["error"] = repr(error)
            raise
        finally:
            stem.with_suffix(".json").write_text(
                json.dumps(result, indent=2) + "\n",
            )
            d.close()
            e.close()
    print(
        json.dumps({"result": result["result"], "capture": str(stem)}),
        flush=True,
    )


if __name__ == "__main__":
    main()
