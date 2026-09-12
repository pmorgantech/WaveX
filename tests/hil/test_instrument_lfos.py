"""Instrument LFO transport, revision checks and disk persistence."""

import struct
import time

import pytest
from test_oscillators import _instrument, _wait_osc
from test_sequencer_tracks import (  # noqa: F401
    sequence_samples as sequence_samples_fixture,
)


def _lfo(daisy, request, revision, index, values):
    daisy.msg(
        0x6E,
        struct.pack(  # noqa: E501
            "<IIBBBBBBBBfff", request, revision, 0, index, 1, 0, *values
        ),
    )


@pytest.mark.daisy
@pytest.mark.sdcard
def test_two_voice_lfos_survive_wxi_recall(daisy, sequence_samples):
    daisy.bind_track(0, sequence_samples[0])
    before = daisy.cmd("LFO", 0, 1)
    values = (4, 0, 0, 1, 5.25, 0.125, 0.75)
    _lfo(daisy, 880001, int(before["revision"]), 1, values)
    edited = daisy.cmd("LFO", 0, 1)
    assert edited["completed"] == "880001" and edited["error"] == "0"
    for key, value in {
        "wave": 4,
        "sync": 0,
        "retrigger": 0,
        "follow": 1,
        "rate": 5250,
        "delay": 125,
        "fade": 750,
    }.items():
        assert edited[key] == str(value)
    _lfo(daisy, 880001, int(before["revision"]), 1, values)
    assert daisy.cmd("LFO", 0, 1)["revision"] == edited["revision"]
    _lfo(daisy, 880002, int(before["revision"]), 0, values)
    assert daisy.cmd("LFO", 0, 0)["error"] == "1"
    _lfo(  # noqa: E501
        daisy, 880003, int(edited["revision"]), 0, (0, 7, 1, 0, 2.0, 0.0, 0.2)
    )
    assert daisy.cmd("LFO", 0, 0)["sync"] == "7"
    name = "HIL LFO " + str(int(time.time()))
    _instrument(daisy, 880004, 5, name)
    _wait_osc(daisy, 0, 0, busy=0, completed=880004, error=0)
    daisy.bind_track(0, 0)
    _instrument(daisy, 880005, 2, "0:/wavex/instruments/" + name + ".wxi")
    _wait_osc(daisy, 0, 0, busy=0, valid=1, zones=1)
    restored = daisy.cmd("LFO", 0, 1)
    for key in (
        "wave",
        "sync",
        "retrigger",
        "follow",
        "rate",
        "delay",
        "fade",
    ):
        assert restored[key] == edited[key]
    assert daisy.cmd("LFO", 0, 0)["sync"] == "7"
