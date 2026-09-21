"""Instrument arp edits share sound Apply/Revert and real note admission."""

import time

import pytest
from test_oscillators import _instrument, _wait_osc
from test_sequencer_tracks import (  # noqa: F401
    sequence_samples as sequence_samples_fixture,
)


@pytest.mark.both
@pytest.mark.sdcard
def test_arp_edits_and_generated_voices(esp32, daisy, sequence_samples):
    esp = esp32
    _instrument(daisy, 854001, 4, "HIL Arp")
    _wait_osc(daisy, 0, 0, completed=854001, error=0)
    _instrument(daisy, 854002, 7, sample=sequence_samples[0])
    _wait_osc(daisy, 0, 0, completed=854002, error=0, zones=1)
    esp.home()
    esp.track(0)
    esp.open_menu("Instrument")
    esp.wait_state(oscready=1, oscvalid=1)
    esp.page("TAB", "Arp")
    esp.wait_state(tab="Arp", arpready=1, arpenabled=0)
    for field, value, key in [
        (0, 1, "arpenabled"),
        (1, 3, "arpmode"),
        (3, 2, "arpoctaves"),
        (5, 1, "arplatch"),
    ]:
        esp.page("ARP", field, value)
        esp.wait_state(
            arpready=1,
            arppending=0,
            editpending=0,
            editdirty=1,
            **{key: value},
        )
    esp.softkey("Revert")
    esp.wait_state(
        arpready=1,
        arpenabled=0,
        arpmode=0,
        arpoctaves=1,
        arplatch=0,
        editdirty=0,
    )
    esp.page("ARP", 0, 1)
    esp.wait_state(arpready=1, arpenabled=1, editpending=0, editdirty=1)
    esp.softkey("Apply")
    esp.wait_state(arpready=1, editdirty=0)
    for note in (60, 64, 67):
        daisy.note(0, note, 100, True)
    time.sleep(0.3)
    # Generated notes use normal admission; listening remains a physical check.
    assert int(daisy.state().get("voices", "0")) > 0
    for note in (60, 64, 67):
        daisy.note(0, note, 0, False)
    esp.page("ARP", 0, 0)
    esp.wait_state(arpready=1, arpenabled=0, editpending=0, editdirty=1)
    esp.softkey("Apply")
    esp.wait_state(editdirty=0)
