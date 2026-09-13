#!/usr/bin/env python3
"""Load a fixed-tempo drum/chord pattern through the HIL console.

Run inside the hardware devcontainer with the serial loggers active. Replaces
session Track bindings and pattern contents; leaves playback running. Source
WAVs are not rewritten. Requires a debug image with the SAMPLE metadata verb.
"""

import argparse
import json
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests/hil"))
from wavex_target import Daisy, Esp32  # noqa: E402

SYNTH = "/99 - Vintage Sound Library/Minimoog/Samples/Saw_Synth_Bass/"
# Zero-based sixteenth-note positions in a two-bar, 120 BPM pattern.
# The card has hats, rims and a clav; use its third hat and ghost snare as
# the remaining percussion lanes, rather than mislabelling them clap/tom.
DRUMS = [
    (
        "Kick",
        "/Drums/Kicks/bassdr01.wav",
        0.225,
        [0, 6, 8, 14, 16, 22, 24, 30],
        110,
    ),
    ("Snare", "/Drums/Snares/606snare.wav", 0.2, [4, 12, 20, 28], 104),
    ("Rim", "/Drums/Rims/Kr55rim.wav", 0.1, [0, 10, 16, 26], 78),
    ("CHat", "/Drums/Hats/Intkit - 01.wav", 0.075, list(range(0, 32, 2)), 64),
    ("OHat", "/Drums/Hats/Intkit - 02.wav", 0.110, [7, 23], 74),
    ("HatFx", "/Drums/Hats/Intkit - 04.wav", 0.080, [3, 11, 19, 27], 45),
    ("Clav", "/Drums/Latin/Kr55clav.wav", 0.090, [0, 5, 13, 16, 21, 29], 58),
    ("Ghost", "/Drums/Snares/snare02.wav", 0.100, [15, 31], 42),
]
CHORDS = [
    ("ChordC", SYNTH + "C2.wav", 48, 48),
    ("ChordE", SYNTH + "E2.wav", 52, 52),
    ("ChordG", SYNTH + "G2.wav", 55, 55),
    ("ChordB", SYNTH + "C3.wav", 60, 59),
]


def setup(daisy, esp):
    def msg(kind, fmt, *args):
        return daisy.msg(kind, struct.pack(fmt, *args))

    def op(kind, track=0, step=0, value8=0, value16=0):
        return msg(0x51, "<BBBBHh", kind, track, step, value8, value16, 0)

    def transport(play):
        return msg(0x50, "<BBBBHH", int(play), 0, 0, 0, 12000, 0)

    def instrument_op(track, operation, name="", sample=0):
        return msg(
            0x60,
            "<IBBH256sBBBhBBBBH",
            4600000 + track * 10 + operation,
            track,
            operation,
            0,
            name.encode(),
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            sample,
        )

    def load(path, request):
        before = set(daisy.samples())
        daisy.load_sample(request, path)
        deadline = time.monotonic() + 20
        while not (added := set(daisy.samples()) - before):
            if time.monotonic() >= deadline:
                raise RuntimeError(f"Sample did not load: {path}")
            time.sleep(0.1)
        assert len(added) == 1, added
        sid = added.pop()
        return sid, daisy.cmd("SAMPLE", sid)

    def sound(track, gain):
        state = daisy.cmd("EDIT", track)
        msg(
            0x80,
            "<IIBBHffff",
            4700000 + track,
            int(state["revision"]),
            track,
            4,
            0,
            20000.0,
            0.0,
            gain,
            0.5,
        )
        state = daisy.cmd("ENV", track, 0)
        msg(
            0x6C,
            "<IIBBBBffffBBhBB",
            4800000 + track,
            int(state["revision"]),
            track,
            0,
            1,
            0,
            0.001,
            0.02,
            1.0,
            0.0,
            0,
            0,
            0,
            0,
            0,
        )
        state = daisy.cmd("ENV", track, 0)
        assert state["release"] == "0" and state["sustain"] == "1000", state

    record = {
        "tempo": 120,
        "steps": 32,
        "tracks": [],
        "quarter_seconds": 0.5,
        "description": (
            "Eight one-shot drum Tracks plus four Cmaj7 chord-note Tracks; "
            "no sample loops or SD preview"
        ),
    }
    transport(False)
    msg(0x33, "<I", 0)
    for track in range(16):
        daisy.unbind_track(track)
        op(10, track)
        op(4, track, value8=int(track < 12))
    daisy.reset_samples()
    daisy.wait_state(voices=0, streaming=0)
    daisy.cmd("FILTER", "wavex", 12, 0)
    op(5, value16=32)
    op(6, value8=1)
    op(7, value8=50)
    for track in range(12):
        chord = track >= 8
        if chord:
            name, path, root, note = CHORDS[track - 8]
            seconds, steps, velocity = 0.5, [0, 16], 80
        else:
            name, path, seconds, steps, velocity = DRUMS[track]
            root = note = 60
        sid, original = load(path, 1200 + track)
        rate = int(original["rate"])
        ratio = 2 ** ((note - root) / 12)
        wanted = round(rate * seconds * ratio)
        end = min(int(original["frames"]), wanted)
        if chord:
            assert end == wanted, (path, original, wanted)
        msg(0x3C, "<HBBhIIIIHH", sid, 0, 0, 0, 0, end, 0, end, 1, 5)
        applied = daisy.cmd("SAMPLE", sid)
        assert applied["loop"] == "0" and int(applied["end"]) == end, applied
        instrument_op(track, 9 if chord else 4, name)
        time.sleep(0.15)
        if chord:
            state = daisy.cmd("OSC", track, 0)
            msg(
                0x68,
                "<IIBBBBHHBBBBB",
                5000000 + track,
                int(state["revision"]),
                track,
                0,
                2,
                0,
                0,
                sid,
                0,
                127,
                1,
                127,
                root,
            )
        else:
            instrument_op(track, 7, sample=sid)
        state = daisy.cmd("OSC", track, 0)
        assert state["zones"] == "1" and state["error"] == "0", state
        sound(track, 0.16 if chord else 0.30)
        for step in steps:
            op(0, track, step, 1, velocity)
            op(11, track, step, note)
        record["tracks"].append(
            {
                "track": track,
                "name": name,
                "path": path,
                "sample_id": sid,
                "root": root,
                "note": note,
                "steps": steps,
                "velocity": velocity,
                "duration_seconds": end / rate / ratio,
                "original": original,
                "applied": applied,
            }
        )
        print(json.dumps({"loaded": record["tracks"][-1]}), flush=True)
    # Account for tails and the loop boundary before starting. One extra output
    # frame conservatively includes the sample-end transition; no gate queue.
    events = []
    for row in record["tracks"]:
        for bar_copy in (-4.0, 0.0, 4.0):
            for step in row["steps"]:
                begin = bar_copy + step * 0.125
                end = begin + row["duration_seconds"] + 1 / 48000
                events.extend(((begin, 1), (end, -1)))
    active = peak = 0
    for at, delta in sorted(events):
        active += delta
        if 0 <= at < 4:
            peak = max(peak, active)
    assert peak <= 8, f"Arrangement would steal voices: {peak}"
    record["maximum_overlap_bound"] = peak
    # Verify every lane sounds individually and ends without a host note-off.
    for row in record["tracks"]:
        daisy.note(row["track"], row["note"], row["velocity"])
        daisy.wait_state(voices=0, timeout=2)
    transport(True)
    esp.track(0)
    esp.open_menu("Sequencer")
    record["ui"] = esp.wait_state(
        seqready=1,
        seqplaying=1,
        seqlen=32,
        seqswing=50,
    )
    record["state"] = daisy.state()
    assert record["state"]["underruns"] == "0", record["state"]
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--label", default="drum-chords")
    args = parser.parse_args()
    daisy, esp = Daisy(), Esp32()
    try:
        record = setup(daisy, esp)
        record["result"] = "playing"
        out = ROOT / "logs" / (args.label + "-arrangement.json")
        out.write_text(json.dumps(record, indent=2) + "\n")
        print(
            json.dumps(
                {
                    "result": "playing",
                    "overlap_bound": record["maximum_overlap_bound"],
                    "record": str(out),
                }
            ),
            flush=True,
        )
    finally:
        daisy.close()
        esp.close()


if __name__ == "__main__":
    main()
