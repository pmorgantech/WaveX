#!/usr/bin/env python3
"""Measure a full eight-channel mix on a QSPI profiling image.

Replaces the bench session and writes uniquely named pattern save copies.
Source WAVs are untouched. Run with the existing serial loggers active.
This is digital-path/callback evidence, not analog or listening verification.
"""

import argparse
import hashlib
import itertools
import json
import re
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests/hil"))
from wavex_target import Daisy, Esp32  # noqa: E402

REQUESTS = itertools.count(int(time.time() * 1000) & 0x7FFFFFFF)
ROUTES = [(4, 1), (6, 6), (17, 7), (16, 4), (3, 1), (1, 2), (7, 3), (17, 5)]


def send(daisy, kind, fmt, *values):
    return daisy.msg(kind, struct.pack(fmt, *values))


def accepted(state, request):
    assert state["completed"] == str(request), state
    assert state["error"] == "0", state


def osc(daisy, track, index, mono, op=1):
    state = daisy.cmd("OSC", track, index)
    request = next(REQUESTS)
    send(
        daisy,
        0x6A,
        "<IIBBBBffbbBB",
        request,
        int(state["revision"]),
        track,
        index,
        op,
        0,
        1.0,
        0.5,
        0,
        17 if index else 0,
        1,
        int(mono),
    )
    state = daisy.cmd("OSC", track, index)
    accepted(state, request)
    return state


def filter_edit(daisy, track, topology, high):
    state = daisy.cmd("EDIT", track)
    request = next(REQUESTS)
    send(
        daisy,
        0x80,
        "<IIBBHffffBBBBf",
        request,
        int(state["revision"]),
        track,
        5,
        (topology << 8) | (3 if high else 1),
        10000.0 if high else 2000.0,
        0.68,
        1.0,
        0.5,
        1,
        0,
        0,
        0,
        1.0,
    )
    state = daisy.cmd("EDIT", track)
    accepted(state, request)
    return state


def configure(daisy, track, mono, topology):
    states = {"osc": [], "env": [], "lfo": [], "routes": []}
    states["osc"].append(osc(daisy, track, 0, mono))
    osc(daisy, track, 1, mono, op=2)
    states["osc"].append(osc(daisy, track, 1, mono))
    states["filter"] = filter_edit(daisy, track, topology, False)
    send(daisy, 0x78, "<BBH", 1, track, 4200)  # -18 dB per Track.
    send(daisy, 0x78, "<BBH", 2, track, 32768)
    for index in (1, 2):
        state = daisy.cmd("ENV", track, index)
        request = next(REQUESTS)
        send(
            daisy,
            0x6C,
            "<IIBBBBffffBBhBB",
            request,
            int(state["revision"]),
            track,
            index,
            1,
            0,
            0.031,
            0.17,
            0.6,
            0.3,
            0,
            0,
            0,
            0,
            0,
        )
        state = daisy.cmd("ENV", track, index)
        accepted(state, request)
        states["env"].append(state)
    for index in (0, 1):
        state = daisy.cmd("LFO", track, index)
        request = next(REQUESTS)
        send(
            daisy,
            0x6E,
            "<IIBBBBBBBBfff",
            request,
            int(state["revision"]),
            track,
            index,
            1,
            0,
            0,
            0,
            1,
            0,
            20.0,
            0.001,
            0.003,
        )
        state = daisy.cmd("LFO", track, index)
        accepted(state, request)
        states["lfo"].append(state)
    for slot, (source, destination) in enumerate(ROUTES):
        state = daisy.cmd("MOD", track, slot)
        request = next(REQUESTS)
        send(
            daisy,
            0x6C,
            "<IIBBBBffffBBhBB",
            request,
            int(state["revision"]),
            track,
            slot,
            2,
            0,
            0.0,
            0.0,
            0.0,
            0.0,
            source,
            destination,
            8000,
            1 + slot % 2,
            0,
        )
        state = daisy.cmd("MOD", track, slot)
        accepted(state, request)
        states["routes"].append(state)
    return states


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--stereo",
        type=int,
        choices=range(5),
        required=True,
    )
    parser.add_argument("--seconds", type=float, default=605)
    parser.add_argument(
        "--topology",
        choices=("svf", "ladder"),
        default="ladder",
    )
    parser.add_argument("--sample", default="/03 Lips of Ashes.wav")
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument(
        "--commit", required=True, help="Source commit used to build --image"
    )
    args = parser.parse_args()
    if args.seconds <= 0:
        parser.error("--seconds must be positive")
    stamp = time.strftime("%Y%m%d-%H%M%S")
    stem = ROOT / "logs" / f"stereo-{args.stereo}-{args.topology}-{stamp}"
    mono = 8 - 2 * args.stereo
    voices = mono + args.stereo
    d, e = Daisy(), Esp32()
    offset = None
    data = {
        "source_commit": args.commit,
        "image_sha256": hashlib.sha256(args.image.read_bytes()).hexdigest(),
        "stereo": args.stereo,
        "mono": mono,
        "voices": voices,
        "channels": 8,
        "topology": args.topology,
        "seconds_requested": args.seconds,
        "sample": args.sample,
        "states": [],
        "file_cycles": [],
        "scenario": (
            "two oscillators per voice from the same stereo PCM, "
            "Osc2 +17 cents; full-drive 24 dB filter; three envelopes; "
            "two 20 Hz sine LFOs; eight routes per Instrument; "
            "four locks per enabled step; sequencer; looped SD audition; "
            "live filter edits; pattern save/load every 60 seconds"
        ),
    }

    def transport(play):
        send(d, 0x50, "<BBBBHH", int(play), 0, 0, 0, 12000, 0)

    def pattern_op(op, track=0, step=0, value8=0, value16=0):
        send(d, 0x51, "<BBBBHh", op, track, step, value8, value16, 0)

    def persist():
        if offset is not None:
            with open(d.logfile, "rb") as fh:
                fh.seek(offset)
                stem.with_suffix(".log").write_bytes(fh.read())
        stem.with_suffix(".json").write_text(json.dumps(data, indent=2) + "\n")

    try:
        assert d.probe() and e.probe()
        transport(False)
        send(d, 0x33, "<I", 0)
        for track in range(16):
            d.note(track, 60, on=False)
            d.unbind_track(track)
        d.reset_samples()
        for track in range(16):
            request = next(REQUESTS)
            send(
                d,
                0x60,
                "<IBBH256sBBBhBBBBH",
                request,
                track,
                9,
                0,
                b"Channel bench",
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
            )
            deadline = time.monotonic() + 15
            while True:
                state = d.cmd("OSC", track, 0)
                if state["completed"] == str(request) and state["busy"] == "0":
                    accepted(state, request)
                    break
                assert time.monotonic() < deadline, state
                time.sleep(0.05)
            d.unbind_track(track)
        send(d, 0x78, "<BBH", 5, 0, 0)
        send(d, 0x78, "<BBH", 8, 0, 0)
        e.track(0)
        e.open_menu("Sample")
        e.page("TAB", "Browse")
        e.page("DIR", str(Path(args.sample).parent))
        e.wait_state(entries=lambda n: int(n) > 1)
        e.page("SEL", Path(args.sample).name)
        e.wait_state(sel=Path(args.sample).name.replace(" ", "_"))
        e.softkey("Load")
        loaded = e.wait_state(
            status=lambda v: "loaded_onto_Track" in v,
            timeout=40,
        )
        sid = int(loaded["lastid"])
        meta = d.cmd("SAMPLE", sid)
        rate = int(meta["rate"])
        assert int(meta["frames"]) > rate * 11, meta
        send(
            d,
            0x3C,
            "<HBBhIIIIHH",
            sid,
            1,
            0,
            0,
            rate * 10,
            rate * 11,
            rate * 10,
            rate * 11,
            0,
            0,
        )
        data["instruments"] = []
        for track in range(voices):
            d.bind_track(track, sid)
            data["instruments"].append(
                configure(
                    d,
                    track,
                    track >= args.stereo,
                    int(args.topology == "ladder"),
                )
            )
        pattern_op(5, value16=16)
        pattern_op(7, value8=60)
        for track in range(16):
            pattern_op(4, track, value8=int(track < voices))
            for step in range(16):
                enabled = int(track < voices and step % 2 == 0)
                pattern_op(0, track, step, enabled, 100)
                pattern_op(11, track, step, 60)
                if track < voices and step % 2 == 0:
                    for parameter, value in (
                        (2, 50000),
                        (3, 45000),
                        (8, 32768),
                        (9, 40000),
                    ):
                        pattern_op(8, track, step, parameter, value)
        stream_offset = Path(d.logfile).stat().st_size
        send(d, 0x4B, "<H", sid)
        transport(True)
        e.open_menu("Sequencer")
        e.wait_state(seqready=1, seqplaying=1)
        time.sleep(8)
        baseline = d.wait_state(voices=voices, streaming=1)
        assert baseline["underruns"] == "0", baseline
        assert baseline["dropped"] == "0", baseline
        data["baseline"] = baseline
        data["stream"] = d.cmd("STREAM")
        with open(d.logfile, "rb") as fh:
            fh.seek(stream_offset)
            opened = fh.read().decode(errors="replace")
        geometry = re.search(
            r"WAV open: .* (\d+)Hz ch=(\d+) bits=(\d+) ",
            opened,
        )
        assert geometry and geometry.group(2, 3) == ("2", "16"), opened
        data["source_format"] = dict(
            zip(("rate", "channels", "bits"), map(int, geometry.groups()))
        )
        offset = Path(d.logfile).stat().st_size
        start = time.monotonic()
        data["started_utc"] = time.strftime(
            "%Y-%m-%dT%H:%M:%SZ",
            time.gmtime(),
        )
        tick, next_file, next_report = 0, 60.0, 0.0
        print(
            json.dumps(
                {"started": str(stem), "voices": voices, "channels": 8},
            ),
            flush=True,
        )
        while time.monotonic() - start < args.seconds:
            filter_edit(
                d,
                tick % voices,
                int(args.topology == "ladder"),
                bool(tick % 2),
            )
            elapsed = time.monotonic() - start
            if elapsed >= next_file:
                e.key("SHIFT")
                e.softkey("Files")
                e.wait_state(fileready=1)
                # Pattern names have 23 usable characters. Keep date and time.
                compact_stamp = stamp[2:].replace("-", "")
                name = f"SC {compact_stamp} {int(next_file)}"
                e.page("NAME", name)
                e.softkey("Save copy")
                saved = e.wait_state(
                    fileready=1,
                    fileerror=0,
                    filename=name.replace(" ", "_"),
                    sk2="Load",
                    sk2en=1,
                    timeout=15,
                )
                e.softkey("Load")
                e.wait_state(fileconfirm=2)
                e.softkey("Confirm")
                e.wait_state(
                    fileready=1,
                    fileerror=0,
                    fileconfirm=0,
                    timeout=15,
                )
                send(d, 0x4B, "<H", sid)
                transport(True)
                e.softkey("Back")
                e.wait_state(seqready=1, seqplaying=1)
                d.wait_state(voices=voices, streaming=1)
                data["file_cycles"].append(
                    {"elapsed": elapsed, "name": name, "saved": saved}
                )
                next_file += 60
            state = d.state()
            assert state["voices"] == str(voices), state
            assert state["streaming"] == "1", state
            assert state["underruns"] == baseline["underruns"], state
            assert state["dropped"] == "0", state
            if elapsed >= next_report:
                data["states"].append({"elapsed": elapsed, **state})
                persist()
                print(json.dumps(data["states"][-1]), flush=True)
                next_report += 15
            tick += 1
            time.sleep(1)
        data["duration_actual"] = time.monotonic() - start
        data["result"] = "passed"
    except BaseException as error:
        data["result"] = "failed"
        data["error"] = str(error)
        raise
    finally:
        persist()
        try:
            transport(False)
            send(d, 0x33, "<I", 0)
            for track in range(16):
                d.note(track, 60, on=False)
            e.home()
        finally:
            d.close()
            e.close()
        print(
            json.dumps({"result": data["result"], "capture": str(stem)}),
            flush=True,
        )


if __name__ == "__main__":
    main()
