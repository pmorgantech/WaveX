"""Whole-note admission in the live and sequenced sampler paths."""

import pytest
from test_key_map import open_keys, stage
from test_sequencer_tracks import _pattern, _transport
from test_sequencer_tracks import (  # noqa: F401
    sequence_samples as sequence_samples_fixture,
)


@pytest.mark.both
@pytest.mark.sdcard
@pytest.mark.parametrize("sequenced", [False, True])
def test_stealing_a_four_layer_note_retires_all_its_layers(
    esp32, daisy, sequence_samples, sequenced
):
    a, b = sequence_samples
    esp = esp32
    open_keys(esp, 0)
    esp.softkey("New keys")
    esp.wait_state(keyview=3)
    esp.softkey("Confirm")
    esp.wait_state(keyview=1)
    esp.page("NAME", "HIL note groups")
    esp.softkey("Confirm")
    esp.wait_state(keyready=1, keyeditable=1)
    for zone in range(1, 5):
        esp.page("ZONE", zone)
        esp.wait_state(keyzone=zone)
        esp.softkey("Assign")
        esp.wait_state(keyview=2, keypick0=a)
        esp.page("CHOOSE", 1)
        esp.wait_state(keyready=1, keysample=a)
        stage(esp, KEYLO=48, KEYHI=72, VELLO=1, VELHI=127, ROOT=60)
    daisy.bind_track(1, b)
    daisy.bind_track(2, b)
    try:
        if sequenced:
            _pattern(daisy, (0,))
            _transport(daisy, True)
        else:
            daisy.note(0, 60)
        daisy.wait_state(voices=4)
        _transport(daisy, False)
        for count, note in enumerate(range(61, 65), 5):
            daisy.note(1, note)
            daisy.wait_state(voices=count)
        # One new mono voice needs only one channel, but all four layers of
        # the oldest musical note go together. Per-layer stealing leaves 8.
        daisy.note(2, 60)
        daisy.wait_state(voices=5)
        daisy.note(0, 60, on=False)
        assert daisy.state()["voices"] == "5"
        for count, note in zip(range(4, 0, -1), range(61, 65)):
            daisy.note(1, note, on=False)
            daisy.wait_state(voices=count)
        daisy.note(2, 60, on=False)
        daisy.wait_state(voices=0)
        assert daisy.state()["underruns"] == "0"
    finally:
        _transport(daisy, False)
        for track in (0, 1, 2):
            daisy.unbind_track(track)
        esp.home()
