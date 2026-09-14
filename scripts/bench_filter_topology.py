#!/usr/bin/env python3
"""Eight held voices through one filter topology, for the callback gate.

Run with the serial loggers active and a profiling Daisy image
(docs/performance_monitoring.md). Binds Tracks 0-7 to a full-file-looped
kick, sets every Instrument's filter to the chosen topology (LP, 24 dB and
full drive, all Instrument-owned), routes a very slow triangle voice LFO and
the long Env 2 / Env 3 to cutoff and resonance through four matrix slots,
holds
one note per Track and alternates the cutoff on all eight Tracks about every
1.2 s for --seconds, as the 2026-09-07 topology captures did. Leaves the
Daisy with the notes released and the samples unloaded. Evaluate the
capture with `make perf-eval`.

  bench_filter_topology.py --topology ladder            # 200 s
  bench_filter_topology.py --topology svf --seconds 600
"""

import argparse
import json
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests/hil"))
from wavex_target import Daisy  # noqa: E402

KICK = "/Drums/Kicks/bassdr01.wav"
TOPOLOGIES = {"svf": 0, "ladder": 1}
TRACKS = range(8)
PARAM_FILTER_CUTOFF = 2
# The backend drops an edit whose request id it has already completed, so
# ids must differ between runs, not just within one.
RUN_ID = (int(time.time()) % 40000) * 100000
# mod_matrix.hpp ids: sources SRC_ENV_FILTER 3, SRC_LFO_VOICE 6,
# SRC_ENV_AUX 16; destinations DEST_CUTOFF 1, DEST_RESONANCE 5.
ROUTES = [(6, 1, 20000), (6, 5, 10000), (3, 1, 24000), (16, 5, 14000)]


def load_looped(daisy, path):
    before = set(daisy.samples())
    daisy.load_sample(1300, path)
    deadline = time.monotonic() + 20
    while not (added := set(daisy.samples()) - before):
        if time.monotonic() >= deadline:
            raise RuntimeError(f"Sample did not load: {path}")
        time.sleep(0.1)
    sid = added.pop()
    # Full-file loop, as the earlier captures used.
    daisy.msg(
        0x3C,
        struct.pack("<HBBhIIIIHH", sid, 1, 0, 0, 0, 0, 0, 0, 0, 0),
    )
    return sid


def set_filter(daisy, track, topology, cutoff_hz, resonance, slope, drive):
    state = daisy.cmd("EDIT", track)
    daisy.msg(
        0x80,
        struct.pack(
            "<IIBBHffffBBBBf",
            RUN_ID + 1000 + track,
            int(state["revision"]),
            track,
            5,
            topology << 8,  # filter_type LP in the low byte
            cutoff_hz,
            resonance,
            1.0,
            0.5,
            slope,  # 0 = 12 dB, 1 = 24 dB
            0,
            0,
            0,
            drive,  # 0..1
        ),
    )
    state = daisy.cmd("EDIT", track)
    assert state["topology"] == str(topology), state
    assert state["mode"] == "0", state
    assert state["slope"] == str(slope), state


def configure_modulation(daisy, track):
    """Slow triangle voice LFO 1 plus long Env 2/3 into cutoff/resonance."""
    rev = int(daisy.cmd("LFO", track, 0)["revision"])
    daisy.msg(
        0x6E,
        struct.pack(
            "<IIBBBBBBBBfff",
            RUN_ID + 2000 + track,
            rev,
            track,
            0,  # LFO 1
            1,  # INST_LFO_SET
            0,
            1,  # triangle
            0,  # Hz
            0,  # free-running
            0,
            0.05,
            0.0,
            0.0,
        ),
    )
    state = daisy.cmd("LFO", track, 0)
    assert state["error"] == "0" and state["wave"] == "1", state
    for env in (1, 2):
        rev = int(daisy.cmd("ENV", track, env)["revision"])
        daisy.msg(
            0x6C,
            struct.pack(
                "<IIBBBBffffBBhBB",
                RUN_ID + 3000 + track * 4 + env,
                rev,
                track,
                env,
                1,  # INST_MOD_SET_ENV
                0,
                6.0,
                6.0,
                0.4,
                6.0,
                0,
                0,
                0,
                0,
                0,
            ),
        )
    for slot, (source, dest, depth) in enumerate(ROUTES):
        rev = int(daisy.cmd("MOD", track, slot)["revision"])
        daisy.msg(
            0x6C,
            struct.pack(
                "<IIBBBBffffBBhBB",
                RUN_ID + 4000 + track * 8 + slot,
                rev,
                track,
                slot,
                2,  # INST_MOD_SET_SLOT
                0,
                0.0,
                0.0,
                0.0,
                0.0,
                source,
                dest,
                depth,
                0,
                0,
            ),
        )
    state = daisy.cmd("MOD", track, 0)
    assert state["error"] == "0", state


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--topology", choices=TOPOLOGIES, required=True)
    parser.add_argument("--seconds", type=float, default=200.0)
    parser.add_argument("--no-modulation", action="store_true")
    parser.add_argument("--slope", type=int, default=24, choices=(12, 24))
    parser.add_argument("--drive", type=int, default=100)
    args = parser.parse_args()
    topology = TOPOLOGIES[args.topology]

    daisy = Daisy()
    daisy.probe()
    daisy.msg(0x50, struct.pack("<BBBBHH", 0, 0, 0, 0, 12000, 0))
    for track in range(16):
        daisy.unbind_track(track)
    daisy.reset_samples()
    daisy.wait_state(voices=0, streaming=0)
    sid = load_looped(daisy, KICK)
    for track in TRACKS:
        daisy.bind_track(track, sid)
        set_filter(
            daisy,
            track,
            topology,
            2000.0,
            0.6,
            int(args.slope == 24),
            args.drive / 100.0,
        )
        if not args.no_modulation:
            configure_modulation(daisy, track)
    for track in TRACKS:
        daisy.note(track, 60, 127)
    daisy.wait_state(voices=len(TRACKS))
    print(
        json.dumps(
            {
                "topology": args.topology,
                "slope": args.slope,
                "drive": args.drive,
                "modulation": not args.no_modulation,
                "routes": ROUTES,
                "sample_id": sid,
                "state": daisy.state(),
            }
        ),
        flush=True,
    )

    started = time.monotonic()
    high = True
    while time.monotonic() - started < args.seconds:
        value = 60000 if high else 32768
        for track in TRACKS:
            payload = struct.pack("<BBH", PARAM_FILTER_CUTOFF, track, value)
            daisy.msg(0x01, payload)
        high = not high
        time.sleep(1.2)

    final = daisy.state()
    for track in TRACKS:
        daisy.note(track, 60, 0, on=False)
    daisy.wait_state(voices=0, timeout=10)
    daisy.reset_samples()
    print(json.dumps({"final": final}), flush=True)
    if final.get("underruns") != "0":
        print("underruns reported", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
