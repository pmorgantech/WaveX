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
    esp.page("NOTE", 75)
    esp.wait_state(seqready=1, seqnote=75)
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
    esp.wait_state(seqready=1, seqvel=77, seqprob=63, seqbits=mask, seqnote=75)
    esp.softkey("Stop")
    esp.wait_state(seqplaying=0)
    esp.key("SHIFT")
    esp.wait_state(shift=1)
    esp.softkey("Clear row")
    st = esp.wait_state(shift=0)
    assert "Confirm" in esp.softkeys(st), st
    esp.softkey("Confirm")
    esp.wait_state(seqready=1, seqbits=0, seqvel=100, seqprob=100, seqnote=60)
    esp.page("FOCUS", 1, 1)
    esp.wait_state(seqready=1)
    esp.page("LENGTH", 16)
    esp.wait_state(seqready=1, seqlen=16)
    esp.page("TEMPO", 12000)
    esp.wait_state(seqready=1, seqtempo=12000)
    esp.home()


@pytest.mark.both
def test_grid_touch_down_edits_once_and_survives_exit_while_held(esp32, daisy):
    esp = esp32
    esp.open_menu("Sequencer")
    esp.wait_state(seqready=1)
    esp.page("FOCUS", 1, 1)
    before = esp.wait_state(seqready=1, seqtrack=1, seqpage=1)
    original = int(before["seqbits"])
    xy = esp.page("CELL", 1, 1)
    x, y = int(xy["x"]), int(xy["y"])
    try:
        esp.cmd("TOUCH", "DOWN", x, y)
        # No UP has been sent: a release-driven implementation fails here.
        esp.wait_state(seqready=1, seqbits=original ^ 1)
        deadline = time.monotonic() + 0.7
        while time.monotonic() < deadline:
            assert int(esp.state()["seqbits"]) == original ^ 1
            time.sleep(0.05)
        esp.cmd("TOUCH", "UP", x, y)
        time.sleep(0.15)
        esp.wait_state(seqready=1, seqbits=original ^ 1)
        # The next press works after readback; leaving while held must not
        # deliver a late release to a deleted cell or toggle a second time.
        esp.cmd("TOUCH", "DOWN", x, y)
        esp.wait_state(seqready=1, seqbits=original)
        esp.home()
        esp.wait_state(page="Main_Menu")
    finally:
        esp.cmd("TOUCH", "UP", x, y)
    esp.open_menu("Sequencer")
    esp.wait_state(seqready=1)
    esp.page("FOCUS", 1, 1)
    esp.wait_state(seqready=1, seqbits=original)
    daisy.wait_state(underruns=0, dropped=0)
    esp.home()
