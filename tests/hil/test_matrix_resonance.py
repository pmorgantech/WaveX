"""Resonance routing uses the common live Instrument undo/save path."""

import time

import pytest
from test_oscillators import _instrument, _wait_osc
from test_sequencer_tracks import (  # noqa: F401
    sequence_samples as sequence_samples_fixture,
)


@pytest.mark.both
@pytest.mark.sdcard
def test_resonance_route_preview_undo_and_wxi(esp32, daisy, sequence_samples):
    esp = esp32
    _instrument(daisy, 899001, 9, "HIL resonance matrix")
    _wait_osc(daisy, 0, 0, completed=899001, error=0)
    daisy.bind_track(0, sequence_samples[0])
    _wait_osc(daisy, 0, 0, busy=0, valid=1, zones=1)
    esp.home()
    esp.track(0)
    esp.open_menu("Instrument")
    esp.wait_state(oscready=1, oscvalid=1)
    esp.page("TAB", "Mod")
    esp.wait_state(modready=1)
    esp.page("SLOT", 8)
    daisy.note(0, 60, 127)
    daisy.wait_state(voices=1)
    esp.page("SOURCE", 1)
    esp.page("DEST", 5)
    esp.page("DEPTH", 16384)
    esp.wait_state(editdirty=1, editpending=0, moddirty=0)
    route = daisy.cmd("MOD", 0, 7)
    assert route["destination"] == "5" and route["depth"] == "16384"
    esp.softkey("Apply")
    esp.wait_state(editdirty=0, editpending=0)
    esp.page("DEPTH", -16384)
    esp.wait_state(editdirty=1, editpending=0, moddirty=0)
    assert daisy.cmd("MOD", 0, 7)["depth"] == "-16384"
    esp.softkey("Revert")
    esp.wait_state(editdirty=0, editpending=0, moddepth=16384)
    esp.softkey("Clear")
    esp.wait_state(editdirty=1, editpending=0, moddepth=0)
    assert daisy.cmd("MOD", 0, 7)["destination"] == "0"
    esp.softkey("Revert")
    esp.wait_state(editdirty=0, editpending=0, moddepth=16384)
    daisy.wait_state(voices=1)
    name = "HIL res " + str(int(time.time()))
    _instrument(daisy, 899002, 5, name)
    _wait_osc(daisy, 0, 0, completed=899002, busy=0, error=0)
    esp.home()
    daisy.bind_track(0, 0)
    _instrument(daisy, 899003, 2, "0:/wavex/instruments/" + name + ".wxi")
    _wait_osc(daisy, 0, 0, busy=0, valid=1, zones=1)
    route = daisy.cmd("MOD", 0, 7)
    assert route["destination"] == "5" and route["depth"] == "16384"
