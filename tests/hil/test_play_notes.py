"""Sustained Play touch notes survive latch refreshes and release correctly."""

import time

import pytest
from test_oscillators import _instrument, _wait_osc
from test_sequencer_tracks import (  # noqa: F401
    sequence_samples as sequence_samples_fixture,
)


@pytest.mark.both
@pytest.mark.sdcard
@pytest.mark.parametrize(
    "tab_x,point,other_tab_x,other_point",
    [(320, (130, 185), 955, (40, 500)), (955, (40, 500), 320, (130, 185))],
    ids=["pads", "keys"],
)
def test_play_touch_latch_transpose_and_exit_release_notes(
    esp32, daisy, sequence_samples, tab_x, point, other_tab_x, other_point
):
    # The fixture loops a short resident sample so a missing note-off cannot
    # pass just because a one-shot finished. These coordinates are verified
    # against the fixed 1280x720 Play layout on the bench.
    _instrument(daisy, 896001, 9, "HIL Play touch")
    _wait_osc(daisy, 0, 0, completed=896001, error=0)
    daisy.bind_track(0, sequence_samples[0])
    _wait_osc(daisy, 0, 0, busy=0, valid=1, zones=1)
    esp = esp32
    esp.home()
    esp.track(0)
    esp.open_menu("Play")
    esp.tap(tab_x, 100)
    time.sleep(0.3)
    try:
        esp.cmd("TOUCH", "DOWN", *point)
        daisy.wait_state(voices=1)
        time.sleep(0.7)
        daisy.wait_state(voices=1)
        esp.cmd("TOUCH", "UP", *point)
        daisy.wait_state(voices=0)

        esp.softkey("Latch")
        esp.wait_state(sk5="Latch*")
        esp.tap(*point)
        daisy.wait_state(voices=1)
        time.sleep(0.7)
        daisy.wait_state(voices=1)
        esp.tap(*point)
        daisy.wait_state(voices=0)

        esp.tap(*point)
        daisy.wait_state(voices=1)
        esp.key("SHIFT")
        esp.softkey("Semi +")
        daisy.wait_state(voices=0)
        esp.tap(*point)
        daisy.wait_state(voices=1)
        esp.tap(other_tab_x, 100)
        daisy.wait_state(voices=0)
        esp.tap(*other_point)
        daisy.wait_state(voices=1)
        esp.softkey("Latch*")
        daisy.wait_state(voices=0)

        esp.cmd("TOUCH", "DOWN", *other_point)
        daisy.wait_state(voices=1)
        esp.home()
        daisy.wait_state(voices=0)
        esp.cmd("TOUCH", "UP", *other_point)
        esp.open_menu("Play")
        esp.wait_state(page="Play")
        daisy.wait_state(voices=0, underruns=0, dropped=0)
    finally:
        esp.cmd("TOUCH", "UP", *other_point)
        esp.home()
