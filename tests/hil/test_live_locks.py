"""Motion capture through the wire, with UI feedback and lock readback."""

import struct

import pytest
from test_melodic import _notes
from test_pattern_files import _back, _files, _new


@pytest.mark.both
def test_live_controls_capture_only_armed_track_and_playing_mode(esp32, daisy):
    esp = esp32
    esp.open_menu("Sequencer")
    esp.wait_state(seqready=1)
    _files(esp)
    _new(esp)
    _back(esp)
    esp.page("LENGTH", 1)
    esp.wait_state(seqready=1, seqlen=1)
    _notes(esp)
    esp.page("MODE", 2)
    esp.wait_state(notesready=1, record=2)
    esp.softkey("Play")
    esp.wait_state(notesready=1, playing=1)
    previous_evictions = int(esp.state()["lockevictions"])
    try:
        for param in range(2, 7):
            daisy.msg(0x01, struct.pack("<BBH", param, 0, param * 1000))
        esp.wait_state(
            feedback=1,
            lockevictions=lambda n: int(n) > previous_evictions,
        )
        # A control for another Track cannot overwrite the armed Track.
        daisy.msg(0x01, struct.pack("<BBH", 3, 1, 55555))
    finally:
        daisy.msg(0x50, struct.pack("<BBBBHH", 0, 0, 2, 0, 12000, 0))
    esp.wait_state(notesready=1, playing=0)
    daisy.msg(0x01, struct.pack("<BBH", 3, 0, 44444))
    esp.page("MODE", 0)
    esp.wait_state(notesready=1, record=0)
    esp.softkey("Back")
    esp.wait_state(seqready=1)
    esp.key("SHIFT")
    esp.softkey("Locks")
    esp.wait_state(seqready=1, seqlocks=1)
    for slot, param in enumerate(range(3, 7), 1):
        esp.page("SLOT", slot)
        esp.wait_state(
            seqready=1, lockslot=slot, lockparam=param, lockvalue=param * 1000
        )
    esp.softkey("Grid")
    esp.wait_state(seqlocks=0, seqbits=0)
    esp.home()
    daisy.wait_state(underruns=0, dropped=0)
