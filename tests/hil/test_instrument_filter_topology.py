"""Filter topology (SVF/Ladder) previews on held notes and survives WXI."""

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
def test_filter_topology_preview_revert_apply_and_save(
    esp32, daisy, sequence_samples
):  # noqa: E501
    esp = esp32
    _instrument(daisy, 896001, 9, "HIL filter topology")
    _wait_osc(daisy, 0, 0, completed=896001, error=0)
    daisy.bind_track(0, sequence_samples[0])
    _wait_osc(daisy, 0, 0, busy=0, valid=1, zones=1)
    before = daisy.cmd("EDIT", 0)
    assert before["topology"] == "0"
    # A cutoff above Nyquist is an exact bypass on both topologies, so the
    # held note must keep sounding at the same level across the switch.
    daisy.msg(
        0x80,
        struct.pack(
            "<IIBBHffff",
            896002,
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
    esp.wait_state(editready=1, filtertopology=0)
    esp.key("SHIFT")
    esp.softkey("Apply")
    esp.wait_state(editdirty=0, editpending=0)
    daisy.note(0, 60, 127)
    daisy.wait_state(voices=1)
    assert min(_peak(daisy)) > 100
    esp.page("MODEL", 1)
    esp.wait_state(filtertopology=1, editdirty=1, editpending=0)
    assert daisy.cmd("EDIT", 0)["topology"] == "1"
    assert daisy.cmd("EDIT", 0)["mode"] == "0"
    assert min(_peak(daisy)) > 100
    esp.key("SHIFT")
    esp.softkey("Revert")
    esp.wait_state(filtertopology=0, editdirty=0, editpending=0)
    assert daisy.cmd("EDIT", 0)["topology"] == "0"
    esp.page("MODEL", 1)
    esp.wait_state(filtertopology=1, editdirty=1, editpending=0)
    # A mode edit on top must not put the SVF back.
    esp.page("TYPE", 2)
    esp.wait_state(filtermode=2, filtertopology=1, editpending=0)
    assert daisy.cmd("EDIT", 0)["topology"] == "1"
    esp.key("SHIFT")
    esp.softkey("Apply")
    esp.wait_state(editdirty=0, editpending=0)
    name = "HIL topology " + str(int(time.time()))
    _instrument(daisy, 896003, 5, name)
    _wait_osc(daisy, 0, 0, completed=896003, busy=0, error=0)
    assert daisy.cmd("EDIT", 0)["dirty"] == "0"
    esp.home()
    daisy.bind_track(0, 0)
    _instrument(daisy, 896004, 2, "0:/wavex/instruments/" + name + ".wxi")
    _wait_osc(daisy, 0, 0, busy=0, valid=1, zones=1)
    assert daisy.cmd("EDIT", 0)["topology"] == "1"
    assert daisy.cmd("EDIT", 0)["mode"] == "2"
    esp.open_menu("Instrument")
    esp.wait_state(oscready=1, oscvalid=1)
    esp.page("TAB", "Filter")
    esp.wait_state(editready=1, filtertopology=1, filtermode=2, editdirty=0)
    esp.home()
