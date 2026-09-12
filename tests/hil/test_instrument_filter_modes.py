"""Filter mode preview reaches held DSP and survives WXI save/recall."""

import struct
import time

import pytest
from test_instrument_live import _peak
from test_oscillators import _instrument, _wait_osc
from test_sequencer_tracks import (  # noqa: F401
    sequence_samples as sequence_samples_fixture,
)


@pytest.mark.both
@pytest.mark.sdcard
def test_filter_modes_preview_revert_apply_and_save(
    esp32, daisy, sequence_samples
):  # noqa: E501
    esp = esp32
    _instrument(daisy, 895001, 9, "HIL filter modes")
    _wait_osc(daisy, 0, 0, completed=895001, error=0)
    daisy.bind_track(0, sequence_samples[0])
    _wait_osc(daisy, 0, 0, busy=0, valid=1, zones=1)
    before = daisy.cmd("EDIT", 0)
    # A cutoff above Nyquist gives exact pass/silence assertions on the
    # existing DSP meters when changing the mode of the playing note.
    daisy.msg(
        0x80,
        struct.pack(
            "<IIBBHffff",
            895002,
            int(before["revision"]),
            0,
            3,
            0,
            96000.0,
            0.0,
            1.0,
            0.5,
        ),
    )
    assert daisy.cmd("EDIT", 0)["cutoff"] == "96000"
    esp.home()
    esp.track(0)
    esp.open_menu("Instrument")
    esp.wait_state(oscready=1, oscvalid=1)
    esp.page("TAB", "Filter")
    esp.wait_state(editready=1, filtermode=0)
    esp.key("SHIFT")
    esp.softkey("Apply")
    esp.wait_state(editdirty=0, editpending=0)
    daisy.note(0, 60, 127)
    daisy.wait_state(voices=1)
    assert min(_peak(daisy)) > 100
    esp.page("TYPE", 1)
    esp.wait_state(filtermode=1, editdirty=1, editpending=0)
    assert daisy.cmd("EDIT", 0)["mode"] == "1"
    assert _peak(daisy) == [0, 0]
    esp.key("SHIFT")
    esp.softkey("Revert")
    esp.wait_state(filtermode=0, editdirty=0, editpending=0)
    assert min(_peak(daisy)) > 100
    esp.page("TYPE", 2)
    esp.wait_state(filtermode=2, editdirty=1, editpending=0)
    assert _peak(daisy) == [0, 0]
    esp.key("SHIFT")
    esp.softkey("Apply")
    esp.wait_state(editdirty=0, editpending=0)
    esp.page("TYPE", 3)
    esp.wait_state(filtermode=3, editdirty=1, editpending=0)
    assert min(_peak(daisy)) > 100
    esp.key("SHIFT")
    esp.softkey("Revert")
    esp.wait_state(filtermode=2, editdirty=0, editpending=0)
    assert _peak(daisy) == [0, 0]
    esp.page("TYPE", 3)
    esp.wait_state(filtermode=3, editdirty=1, editpending=0)
    assert min(_peak(daisy)) > 100
    # No further note-on occurred during the entire preview sequence.
    name = "HIL filters " + str(int(time.time()))
    _instrument(daisy, 895003, 5, name)
    _wait_osc(daisy, 0, 0, completed=895003, busy=0, error=0)
    assert daisy.cmd("EDIT", 0)["dirty"] == "0"
    esp.home()
    daisy.bind_track(0, 0)
    _instrument(daisy, 895004, 2, "0:/wavex/instruments/" + name + ".wxi")
    _wait_osc(daisy, 0, 0, busy=0, valid=1, zones=1)
    assert daisy.cmd("EDIT", 0)["mode"] == "3"
    esp.open_menu("Instrument")
    esp.wait_state(oscready=1, oscvalid=1)
    esp.page("TAB", "Filter")
    esp.wait_state(editready=1, filtermode=3, editdirty=0)
    esp.home()
