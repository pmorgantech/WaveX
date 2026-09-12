"""One note stays sounding while Instrument previews and undo reach DSP."""

import time

import pytest
from test_oscillators import _instrument, _wait_osc
from test_sequencer_tracks import (  # noqa: F401
    sequence_samples as sequence_samples_fixture,
)


def _peak(daisy):
    # The existing callback telemetry is a snapshot of one block. Sample enough
    # blocks to include the attack in a looping drum sample's waveform.
    time.sleep(0.05)
    peak = [0, 0]
    until = time.monotonic() + 0.5
    while time.monotonic() < until:
        meter = daisy.cmd("METERS")
        peak[0] = max(peak[0], int(meter["left"]))
        peak[1] = max(peak[1], int(meter["right"]))
        time.sleep(0.01)
    assert daisy.state()["voices"] == "1"
    return peak


def _hard_pan(daisy):
    time.sleep(0.05)
    active = []
    until = time.monotonic() + 0.5
    while time.monotonic() < until:
        m = daisy.cmd("METERS")
        left, right = int(m["left"]), int(m["right"])
        if max(left, right) > 100:
            active.append(min(left, right) == 0)
        time.sleep(0.01)
    assert active, "Expected audible blocks"
    assert daisy.state()["voices"] == "1"
    return all(active)


@pytest.mark.both
@pytest.mark.sdcard
def test_held_note_hears_preview_apply_and_revert(  # noqa: E501
    esp32, daisy, sequence_samples
):
    esp = esp32
    _instrument(daisy, 890001, 9, "HIL live sound")
    _wait_osc(daisy, 0, 0, completed=890001, error=0)
    daisy.bind_track(0, sequence_samples[0])
    _wait_osc(daisy, 0, 0, busy=0, valid=1, zones=1)
    esp.home()
    esp.track(0)
    esp.open_menu("Instrument")
    esp.wait_state(oscready=1, oscvalid=1)
    esp.page("TAB", "Env")
    esp.wait_state(modready=1)
    esp.page("SUSTAIN", 1000)
    esp.wait_state(editdirty=1, moddirty=0, editpending=0)
    esp.softkey("Apply")
    esp.wait_state(editdirty=0, editpending=0)
    daisy.note(0, 60, 127)
    daisy.wait_state(voices=1)
    assert min(_peak(daisy)) > 100
    esp.page("TAB", "Amp")
    esp.wait_state(editready=1)
    esp.page("LEVEL", 0)
    esp.wait_state(editdirty=1, editpending=0, instgain=0)
    assert _peak(daisy) == [0, 0]
    esp.key("SHIFT")
    esp.softkey("Revert")
    esp.wait_state(editdirty=0, editpending=0, instgain=1000)
    assert min(_peak(daisy)) > 100
    esp.page("PAN", 1000)
    esp.wait_state(editdirty=1, editpending=0, instpan=1000)
    left, right = _peak(daisy)
    assert left == 0 and right > 100
    esp.key("SHIFT")
    esp.softkey("Revert")
    esp.wait_state(editdirty=0, editpending=0, instpan=500)
    assert min(_peak(daisy)) > 100
    esp.page("TAB", "Osc")
    esp.wait_state(oscready=1)
    esp.page("LEVEL", 0)
    esp.wait_state(oscdirty=0, editdirty=1, editpending=0, osclevel=0)
    assert _peak(daisy) == [0, 0]
    esp.key("SHIFT")
    esp.softkey("Apply")
    esp.wait_state(editdirty=0, editpending=0)
    esp.page("LEVEL", 1000)
    esp.wait_state(oscdirty=0, editdirty=1, editpending=0, osclevel=1000)
    assert min(_peak(daisy)) > 100
    esp.key("SHIFT")
    esp.softkey("Revert")
    esp.wait_state(oscdirty=0, editdirty=0, editpending=0, osclevel=0)
    assert _peak(daisy) == [0, 0]
    esp.page("LEVEL", 1000)
    esp.wait_state(oscdirty=0, editdirty=1, editpending=0)
    esp.key("SHIFT")
    esp.softkey("Apply")
    esp.wait_state(editdirty=0, editpending=0)
    for env, source in [(2, 3), (3, 16)]:
        esp.page("TAB", "Mod")
        esp.wait_state(modready=1)
        esp.page("SOURCE", source)
        esp.page("DEST", 2)
        esp.page("DEPTH", -32767)
        esp.wait_state(moddirty=0, editdirty=1, editpending=0)
        esp.softkey("Apply")
        esp.wait_state(editdirty=0, editpending=0)
        esp.page("TAB", "Env")
        esp.wait_state(modready=1)
        esp.page("ENV", env)
        esp.page("SUSTAIN", 1000)
        esp.wait_state(moddirty=0, editdirty=1, editpending=0, sustain=1000)
        assert _peak(daisy) == [0, 0]
        esp.softkey("Revert")
        esp.wait_state(editdirty=0, editpending=0, sustain=800)
        assert min(_peak(daisy)) > 100
    for lfo, source in [(1, 6), (2, 17)]:
        esp.page("TAB", "LFO")
        esp.wait_state(lfoready=1)
        esp.page("LFO", lfo)
        esp.page("WAVE", 3)
        esp.page("RATE", 5000)
        esp.page("DELAY", 600000)
        esp.wait_state(lfodirty=0, lfopending=0, editdirty=1, editpending=0)
        esp.softkey("Apply")
        esp.wait_state(editdirty=0, editpending=0)
        esp.page("TAB", "Mod")
        esp.wait_state(modready=1)
        esp.page("SOURCE", source)
        esp.page("DEST", 4)
        esp.page("DEPTH", 32767)
        esp.wait_state(moddirty=0, editdirty=1, editpending=0)
        esp.softkey("Apply")
        esp.wait_state(editdirty=0, editpending=0)
        assert not _hard_pan(daisy)
        esp.page("TAB", "LFO")
        esp.wait_state(lfoready=1, lfo=lfo)
        esp.page("DELAY", 0)
        esp.wait_state(lfodirty=0, lfopending=0, editdirty=1, editpending=0)
        assert _hard_pan(daisy)
        esp.softkey("Revert")
        esp.wait_state(editdirty=0, editpending=0, delay=600000)
        assert not _hard_pan(daisy)
    # No second note-on was sent anywhere above.
    esp.home()
