"""Two-source ownership, revision checks and WXI recall on the Daisy."""

import struct
import time

import pytest
from test_sequencer_tracks import (  # noqa: F401
    sequence_samples as sequence_samples_fixture,
)


def _wait_osc(daisy, track, oscillator, **expected):
    deadline = time.monotonic() + 15
    while True:
        state = daisy.cmd("OSC", track, oscillator)
        matches = []
        for key, value in expected.items():
            matches.append(state.get(key) == str(value))
        if all(matches):
            return state
        assert time.monotonic() < deadline, state
        time.sleep(0.05)


def _instrument(daisy, request, op, name="", sample=0):
    daisy.msg(
        0x60,
        struct.pack(
            "<IBBH256sBBBhBBBBH",
            request,
            0,
            op,
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
        ),
    )


def _osc(daisy, request, revision, op, mix=0.5):
    daisy.msg(
        0x6A,
        struct.pack(
            "<IIBBBBffbbBB",
            request,
            revision,
            0,
            1,
            op,
            0,
            0.75,
            mix,
            12,
            -25,
            1,
            0,
        ),
    )


@pytest.mark.daisy
@pytest.mark.sdcard
def test_two_oscillator_copy_retains_samples_and_survives_wxi_recall(
    daisy, sequence_samples
):
    a, b = sequence_samples
    daisy.bind_track(1, a)
    daisy.note(1, 60)
    daisy.wait_state(voices=1)
    name = "HIL dual " + str(int(time.time()))
    _instrument(daisy, 810001, 4, name)
    _wait_osc(daisy, 0, 0, completed=810001, error=0, busy=0)
    _instrument(daisy, 810002, 7, sample=a)
    primary = _wait_osc(daisy, 0, 0, completed=810002, zones=1, error=0)
    _osc(daisy, 810003, int(primary["revision"]), 2)
    copied = _wait_osc(daisy, 0, 1, completed=810003, zones=1, type=1, error=0)
    _osc(daisy, 810004, int(copied["revision"]), 1)
    edited = _wait_osc(
        daisy,
        0,
        1,
        completed=810004,
        error=0,
        mix=500,
        level=750,
        coarse=12,
        fine=-25,
    )
    # Duplicate delivery cannot apply an edit twice or advance the revision.
    _osc(daisy, 810004, int(copied["revision"]), 1)
    assert daisy.cmd("OSC", 0, 1)["revision"] == edited["revision"]
    _osc(daisy, 810005, int(copied["revision"]), 1, mix=1)
    stale = _wait_osc(daisy, 0, 1, completed=810005, error=1)
    assert stale["mix"] == "500"
    # Replacing Oscillator 1 keeps Oscillator 2's old sample and Track 1 voice.
    _instrument(daisy, 810006, 7, sample=b)
    _wait_osc(daisy, 0, 0, completed=810006, zones=1, error=0)
    assert set(daisy.samples()) == {a, b}
    daisy.wait_state(voices=1)
    daisy.note(0, 60)
    daisy.wait_state(voices=2)  # two sources consume one voice
    _instrument(daisy, 810007, 5, name)
    _wait_osc(daisy, 0, 1, completed=810007, busy=0, error=0)
    daisy.bind_track(0, 0)
    daisy.wait_state(voices=1)
    _instrument(daisy, 810008, 2, "0:/wavex/instruments/" + name + ".wxi")
    _wait_osc(daisy, 0, 1, valid=1, busy=0, zones=1, mix=500, fine=-25)
    daisy.note(0, 60)
    daisy.wait_state(voices=2)
