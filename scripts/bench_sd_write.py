#!/usr/bin/env python3
"""Run scratch-file SD write/readback probes on the attached Daisy.

Requires the debug image, running serial loggers and an otherwise idle storage
session. Each file uses exclusive creation; passed files are removed and failed
files retained. Stops at the first failure so later cases cannot hide recovery
state. Select the bus speed in the image; this script never remounts the card.
"""

import argparse
import fcntl
import json
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests/hil"))
from wavex_target import Daisy, Esp32  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--bytes", type=int, nargs="+", default=[512, 4096, 1048576, 50331648]
    )
    parser.add_argument("--chunk", type=int, choices=[512, 4096], default=4096)
    parser.add_argument("--shift", type=int, choices=[0, 4], default=0)
    parser.add_argument("--prefix", type=int, choices=[0, 44], default=0)
    parser.add_argument("--resident-playback", action="store_true")
    args = parser.parse_args()
    lock = open("/tmp/wavex-sd-write-bench.lock", "w")
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    esp = Esp32()
    daisy = Daisy()
    esp.home()
    assert daisy.probe(), "Daisy logger/console unavailable"
    before = daisy.cmd("SDIO")
    assert (
        before["valid"] == "0"
    ), f"Reboot before testing: pre-existing failure {before}"
    initial = {"initial": before, "state": daisy.cmd("STATE")}
    print(json.dumps(initial), flush=True)
    playing = False
    sample = None
    try:
        if args.resident_playback:
            # Exercise the audio interrupt without adding SD reads.
            empty = initial["state"]["samples"] == "0"
            assert empty, "Use an empty bench session"
            daisy.load_sample(60000, "/03 Lips of Ashes.wav", timeout=30)
            samples = daisy.samples()
            assert len(samples) == 1, samples
            sample = samples[0]
            daisy.bind_track(15, sample)
            daisy.note(15, 60, 100, True)
            playing = True
        for size in args.bytes:
            options = (size, args.chunk, args.shift, args.prefix)
            daisy.cmd("SDTEST", "START", *options)
            deadline = time.monotonic() + 240
            while True:
                result = daisy.cmd("SDTEST", "STATUS", timeout=40)
                if result["busy"] == "0":
                    break
                if time.monotonic() > deadline:
                    raise RuntimeError(f"Probe timed out: {result}")
                time.sleep(0.1)
            failure = daisy.cmd("SDIO")
            print(
                json.dumps(
                    {
                        "test": result,
                        "failure": failure,
                        "registers": daisy.cmd("SDIO", "REGS"),
                        "state": daisy.cmd("STATE"),
                    }
                ),
                flush=True,
            )
            if result["phase"] != "9" or failure["valid"] != "0":
                return 1
        return 0
    finally:
        if playing:
            daisy.note(15, 60, 0, False)
            daisy.unbind_track(15)
        if sample is not None:
            daisy.unload_sample(sample)
        daisy.close()
        esp.close()


if __name__ == "__main__":
    raise SystemExit(main())
