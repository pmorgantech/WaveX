"""Touch sequencer readback and editing on the connected boards."""

import time

import pytest


@pytest.mark.both
def test_sequencer_grid_edits_survive_navigation_and_page_boundaries(
    esp32,
    daisy,
):
    esp = esp32
    esp.open_menu("Sequencer")
    esp.wait_state(seqready=1)
    esp.page("LENGTH", 64)
    esp.wait_state(seqready=1, seqlen=64)
    esp.page("FOCUS", 16, 64)
    before = esp.wait_state(seqready=1, seqtrack=16, seqpage=49)
    mask = int(before["seqbits"]) ^ 0x8000
    esp.page("TOGGLE")
    esp.wait_state(seqready=1, seqbits=mask)
    esp.page("VELOCITY", 77)
    esp.wait_state(seqready=1, seqvel=77)
    esp.page("PROBABILITY", 63)
    esp.wait_state(seqready=1, seqprob=63)
    esp.page("SWING", 64)
    esp.wait_state(seqready=1, seqswing=64)
    esp.page("TEMPO", 13700)
    esp.wait_state(seqready=1, seqtempo=13700)
    esp.softkey("Play")
    esp.wait_state(seqplaying=1)
    esp.home()
    time.sleep(0.3)
    esp.open_menu("Sequencer")
    esp.wait_state(seqready=1, seqplaying=1, seqtempo=13700)
    esp.page("FOCUS", 16, 64)
    esp.wait_state(seqready=1, seqvel=77, seqprob=63, seqbits=mask)
    esp.softkey("Stop")
    esp.wait_state(seqplaying=0)
    esp.key("SHIFT")
    esp.wait_state(shift=1)
    esp.softkey("Clear row")
    st = esp.wait_state(shift=0)
    assert "Confirm" in esp.softkeys(st), st
    esp.softkey("Confirm")
    esp.wait_state(seqready=1, seqbits=0, seqvel=100, seqprob=100)
    esp.page("FOCUS", 1, 1)
    esp.wait_state(seqready=1)
    esp.page("LENGTH", 16)
    esp.wait_state(seqready=1, seqlen=16)
    esp.page("TEMPO", 12000)
    esp.wait_state(seqready=1, seqtempo=12000)
    esp.home()
