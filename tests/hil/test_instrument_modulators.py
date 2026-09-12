"""Instrument-owned envelope/matrix readback and WXI recall."""

import struct
import time

import pytest
from test_oscillators import _instrument, _wait_osc
from test_sequencer_tracks import (  # noqa: F401
    sequence_samples as sequence_samples_fixture,
)


def _edit(daisy, request, revision, op, index, envelope, slot):
    daisy.msg(
        0x6C,
        struct.pack(
            "<IIBBBBffffBBhBB",
            request,
            revision,
            0,
            index,
            op,
            0,
            *envelope,
            *slot,
        ),
    )


@pytest.mark.daisy
@pytest.mark.sdcard
def test_instrument_envelope_three_and_matrix_survive_wxi_recall(
    daisy, sequence_samples
):
    daisy.bind_track(0, sequence_samples[0])
    before = daisy.cmd("ENV", 0, 2)
    envelope = (0.125, 0.25, 0.5, 0.25)
    slot = (16, 3, -25000, 0, 0)
    _edit(daisy, 850001, int(before["revision"]), 1, 2, envelope, slot)
    applied = daisy.cmd("ENV", 0, 2)
    assert applied["completed"] == "850001"
    assert applied["error"] == "0"
    assert applied["attack"] == "125"
    assert applied["sustain"] == "500"
    _edit(daisy, 850001, int(before["revision"]), 1, 2, envelope, slot)
    assert daisy.cmd("ENV", 0, 2)["revision"] == applied["revision"]
    _edit(daisy, 850002, int(before["revision"]), 2, 7, envelope, slot)
    assert daisy.cmd("MOD", 0, 7)["error"] == "1"
    _edit(daisy, 850003, int(applied["revision"]), 2, 7, envelope, slot)
    route = daisy.cmd("MOD", 0, 7)
    assert route["source"] == "16"
    assert route["depth"] == "-25000"
    assert route["error"] == "0"
    name = "HIL envs " + str(int(time.time()))
    _instrument(daisy, 850004, 5, name)
    _wait_osc(daisy, 0, 0, busy=0, completed=850004, error=0)
    daisy.bind_track(0, 0)
    _instrument(daisy, 850005, 2, "0:/wavex/instruments/" + name + ".wxi")
    _wait_osc(daisy, 0, 0, busy=0, valid=1, zones=1)
    assert daisy.cmd("ENV", 0, 2)["attack"] == "125"
    assert daisy.cmd("ENV", 0, 2)["release"] == "250"
    assert daisy.cmd("MOD", 0, 7)["depth"] == "-25000"
