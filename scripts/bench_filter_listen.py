#!/usr/bin/env python3
"""Set up the filter listening test: one Track per topology.

Run with the serial loggers active. Track 1 (Ladder, the zero-delay-
feedback ladder) and Track 2 (SVF) are bound to the same Minimoog saw
sample, trimmed to a 220 ms pluck, with identical filter settings -
low-pass, 24 dB, half drive, resonance 0.7 - differing only in MODEL.
Each has the same 16-step bass line with a different cutoff lock on every
other step and a short filter envelope, so the sweep, the resonance and
the drive are all heard on every bar. The other Tracks are muted rows.
Playback is left running; on the Sequencer page use Shift > Solo on a row
to hear one topology at a time.

  bench_filter_listen.py            # 100 BPM
  bench_filter_listen.py --bpm 90 --cutoff 900 --res 0.7 --drive 0.5
  bench_filter_listen.py --no-locks --no-env   # Filter tab alone shapes it
  bench_filter_listen.py --only 1 --release 0.6  # ring test: the ladder
                                                 # alone, each pluck rings

A locked step plays at its locked cutoff whatever the Filter tab says, and
the Env 2 route opens the filter on every step; --no-locks and --no-env
leave the tab's CUTOFF/RES/SLOPE/DRIVE as the only filter controls. The
ladder self-oscillates from about 74% RES; raise the release so the ring
outlives the pluck (the filter runs on silence through the amp release).
"""

import argparse
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests/hil"))
from wavex_target import Daisy  # noqa: E402

SAMPLE = "/99 - Vintage Sound Library/Minimoog/Samples/Saw_Synth_Bass/C2.wav"
NOTES = [60, 60, 63, 60, 67, 60, 58, 60, 60, 60, 63, 65, 67, 60, 58, 55]
# Cutoff locks (PARAM_FILTER_CUTOFF, 16-bit exponential 20 Hz..20 kHz) on
# every other step: a slow rise then a drop, so each bar sweeps.
CUTOFFS = {
    0: 22000,
    2: 30000,
    4: 38000,
    6: 46000,
    8: 54000,
    10: 60000,
    12: 40000,
    14: 26000,
}
TOPOLOGIES = [("Ladder", 1), ("SVF", 0)]
PARAM_FILTER_CUTOFF = 2
RUN_ID = (int(time.time()) % 40000) * 100000


def seq_op(daisy, op, track=0, step=0, value8=0, value16=0):
    payload = struct.pack("<BBBBHh", op, track, step, value8, value16, 0)
    daisy.msg(0x51, payload)


def transport(daisy, play, bpm):
    daisy.msg(0x50, struct.pack("<BBBBHH", int(play), 0, 0, 0, bpm * 100, 0))


def load_pluck(daisy, path, seconds):
    """Load the sample unlooped and trim it to `seconds` with a short fade,
    so every note ends by itself: a looped sample with no note-off would
    keep its voice alive after the envelope falls silent, and three Tracks
    of those exhaust the eight-voice pool and steal from the soloed one."""
    before = set(daisy.samples())
    daisy.load_sample(1500, path)
    deadline = time.monotonic() + 20
    while not (added := set(daisy.samples()) - before):
        if time.monotonic() >= deadline:
            raise RuntimeError(f"Sample did not load: {path}")
        time.sleep(0.1)
    sid = added.pop()
    info = daisy.cmd("SAMPLE", sid)
    end = min(int(info["frames"]), int(int(info["rate"]) * seconds))
    region = struct.pack("<HBBhIIIIHH", sid, 0, 0, 0, 0, end, 0, end, 1, 8)
    daisy.msg(0x3C, region)
    applied = daisy.cmd("SAMPLE", sid)
    assert applied["loop"] == "0" and int(applied["end"]) == end, applied
    return sid


def set_filter(daisy, track, topology, cutoff, res, drive):
    state = daisy.cmd("EDIT", track)
    daisy.msg(
        0x80,
        struct.pack(
            "<IIBBHffffBBBBf",
            RUN_ID + 1000 + track,
            int(state["revision"]),
            track,
            5,
            topology << 8,  # LP in the low byte
            cutoff,
            res,
            1.0,
            0.5,
            1,  # 24 dB
            0,
            0,
            0,
            drive,
        ),
    )
    state = daisy.cmd("EDIT", track)
    assert state["topology"] == str(topology) and state["slope"] == "1", state


def set_env(daisy, track, index, a, d, s, r):
    rev = int(daisy.cmd("ENV", track, index)["revision"])
    daisy.msg(
        0x6C,
        struct.pack(
            "<IIBBBBffffBBhBB",
            RUN_ID + 2000 + track * 4 + index,
            rev,
            track,
            index,
            1,  # INST_MOD_SET_ENV
            0,
            a,
            d,
            s,
            r,
            0,
            0,
            0,
            0,
            0,
        ),
    )


def set_route(daisy, track, slot, source, dest, depth):
    rev = int(daisy.cmd("MOD", track, slot)["revision"])
    daisy.msg(
        0x6C,
        struct.pack(
            "<IIBBBBffffBBhBB",
            RUN_ID + 3000 + track * 8 + slot,
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bpm", type=int, default=100)
    parser.add_argument("--cutoff", type=float, default=900.0)
    parser.add_argument("--res", type=float, default=0.7)
    parser.add_argument("--drive", type=float, default=0.5)
    parser.add_argument(
        "--no-locks",
        action="store_true",
        help="no per-step cutoff locks",
    )
    parser.add_argument(
        "--no-env",
        action="store_true",
        help="no Env 2 sweep into cutoff",
    )
    parser.add_argument(
        "--release",
        type=float,
        default=0.05,
        help="amp release in seconds; the filter keeps ringing through it "
        "after the pluck ends (voice budget: 0.6 s fills the pool with "
        "one Track at sixteenths, three Tracks need it short)",
    )
    parser.add_argument(
        "--only",
        type=int,
        default=0,
        help="sequence only this Track (1-2); the others stay row-muted",
    )
    args = parser.parse_args()
    lineup = list(TOPOLOGIES)

    daisy = Daisy()
    daisy.probe()
    transport(daisy, False, args.bpm)
    daisy.msg(0x78, struct.pack("<BBH", 5, 0, 0))  # mixer mute mask clear
    for track in range(16):
        daisy.unbind_track(track)
        seq_op(daisy, 10, track)  # clear steps and locks
        wanted = not args.only or track == args.only - 1
        enabled = track < len(lineup) and wanted
        seq_op(daisy, 4, track, value8=int(enabled))
    daisy.reset_samples()
    daisy.wait_state(voices=0, streaming=0)
    sid = load_pluck(daisy, SAMPLE, 0.22)
    seq_op(daisy, 5, value16=16)
    seq_op(daisy, 7, value8=50)
    for track, (name, topology) in enumerate(lineup):
        daisy.bind_track(track, sid)
        set_filter(daisy, track, topology, args.cutoff, args.res, args.drive)
        # The trimmed region is the note; the amp envelope only softens its
        # start, and Env 2 gives every step a short filter sweep on top of
        # its cutoff lock.
        set_env(daisy, track, 0, 0.003, 0.05, 1.0, args.release)
        set_env(daisy, track, 1, 0.005, 0.18, 0.0, 0.1)
        # Env 2 -> cutoff (slot 0); an empty slot clears a previous run's.
        set_route(daisy, track, 0, 3, 1, 0 if args.no_env else 14000)
        for step, note in enumerate(NOTES):
            seq_op(daisy, 0, track, step, 1, 110)
            seq_op(daisy, 11, track, step, note)
            if step in CUTOFFS and not args.no_locks:
                seq_op(
                    daisy,
                    8,
                    track,
                    step,
                    PARAM_FILTER_CUTOFF,
                    CUTOFFS[step],
                )
        print(f"Track {track + 1}: {name}", flush=True)
    transport(daisy, True, args.bpm)
    daisy.wait_state(voices=lambda v: int(v) > 0, timeout=5)
    note = "playing at %d BPM; Sequencer page > Shift > Solo a row" % args.bpm
    print(note, flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
