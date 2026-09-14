"""Sequencer-page Solo mutes every other Track through the mixer mask."""

import struct
import time

import pytest
from test_sequencer_tracks import _pattern, _transport
from test_sequencer_tracks import (  # noqa: F401
    sequence_samples as sequence_samples_fixture,
)


def _peak(daisy):
    """Output peak over half a second; several voices may be sounding."""
    time.sleep(0.05)
    peak = [0, 0]
    until = time.monotonic() + 0.5
    while time.monotonic() < until:
        meter = daisy.cmd("METERS")
        peak[0] = max(peak[0], int(meter["left"]))
        peak[1] = max(peak[1], int(meter["right"]))
        time.sleep(0.01)
    return peak


@pytest.mark.both
@pytest.mark.sdcard
def test_solo_silences_other_tracks_and_unsolo_restores(
    esp32, daisy, sequence_samples
):  # noqa: E501
    esp = esp32
    daisy.msg(0x78, struct.pack("<BBH", 5, 0, 0))  # mute mask clear
    daisy.bind_track(0, sequence_samples[0])
    _pattern(daisy, (0,), length=4)
    _transport(daisy, True)
    daisy.wait_state(voices=lambda v: int(v) > 0)
    time.sleep(0.3)
    assert min(_peak(daisy)) > 100
    esp.home()
    esp.track(0)
    esp.open_menu("Sequencer")
    esp.wait_state(seqready=1, seqplaying=1)
    # Solo is sticky across page visits: start from none.
    esp.page("SOLO", 0)
    esp.wait_state(seqsolo=0)
    # Solo Track 2 (no steps): Track 1 is muted by the mask, so silence.
    esp.page("SOLO", 2)
    esp.wait_state(seqsolo=2)
    time.sleep(0.4)
    assert _peak(daisy) == [0, 0]
    # Move the solo to Track 1: it is the only audible Track and it sounds.
    esp.page("SOLO", 1)
    esp.wait_state(seqsolo=1)
    time.sleep(0.4)
    assert min(_peak(daisy)) > 100
    esp.page("SOLO", 0)
    esp.wait_state(seqsolo=0)
    time.sleep(0.4)
    assert min(_peak(daisy)) > 100
    _transport(daisy, False)
    esp.home()
