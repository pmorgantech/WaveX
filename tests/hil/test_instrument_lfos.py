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


@pytest.mark.both
@pytest.mark.sdcard
def test_touch_lfo_preview_apply_revert_and_save(  # noqa: E501
    esp32, daisy, sequence_samples
):
    esp = esp32
    _instrument(daisy, 881001, 4, "HIL LFO UI")
    _wait_osc(daisy, 0, 0, completed=881001, error=0)
    _instrument(daisy, 881002, 7, sample=sequence_samples[0])
    _wait_osc(daisy, 0, 0, completed=881002, error=0, zones=1)
    esp.home()
    esp.track(0)
    esp.open_menu("Instrument")
    esp.wait_state(oscready=1, oscvalid=1)
    esp.page("TAB", "LFO")
    esp.wait_state(tab="LFO", lfoready=1, lfovalid=1)
    esp.page("LFO", 2)
    before = daisy.cmd("LFO", 0, 1)
    expected = {
        "WAVE": 4,
        "RATE": 5250,
        "SYNC": 0,
        "RETRIGGER": 0,
        "DELAY": 125,
        "FADE": 750,
        "FOLLOW": 1,
    }
    for key, value in expected.items():
        esp.page(key, value)
    esp.wait_state(
        lfo=2,
        lfodirty=0,
        lfopending=0,
        editdirty=1,
        editpending=0,
        rate=5250,
        fade=750,
        follow=1,
    )
    edited = daisy.cmd("LFO", 0, 1)
    for key, value in expected.items():
        assert edited[key.lower()] == str(value)
    esp.page("TAB", "Amp")
    esp.wait_state(tab="Amp", editdirty=1)
    esp.page("TAB", "LFO")
    esp.wait_state(lfo=2, lfoready=1)
    esp.softkey("Revert")
    esp.wait_state(
        lfo=2,
        lfoready=1,
        editdirty=0,
        editpending=0,
        rate=int(before["rate"]),
        wave=int(before["wave"]),
    )
    assert daisy.cmd("LFO", 0, 1)["rate"] == before["rate"]
    for key, value in expected.items():
        esp.page(key, value)
    esp.wait_state(lfodirty=0, lfopending=0, editdirty=1, editpending=0)
    esp.softkey("Apply")
    esp.wait_state(lfo=2, editdirty=0, editpending=0, rate=5250)
    esp.page("RATE", 3000)
    esp.wait_state(  # noqa: E501
        lfodirty=0, lfopending=0, editdirty=1, editpending=0, rate=3000
    )
    name = "HIL LFO UI " + str(int(time.time()))
    _instrument(daisy, 881003, 5, name)
    _wait_osc(daisy, 0, 0, completed=881003, busy=0, error=0)
    assert daisy.cmd("EDIT", 0)["dirty"] == "0"
    esp.home()
    daisy.bind_track(0, 0)
    _instrument(daisy, 881004, 2, "0:/wavex/instruments/" + name + ".wxi")
    _wait_osc(daisy, 0, 0, busy=0, valid=1, zones=1)
    esp.open_menu("Instrument")
    esp.wait_state(oscready=1, oscvalid=1)
    esp.page("TAB", "LFO")
    esp.wait_state(lfoready=1)
    esp.page("LFO", 2)
    esp.wait_state(lfo=2, rate=3000, wave=4, fade=750, follow=1)
    esp.home()
