"""Touch pattern save/load and session ownership on real hardware."""

import struct
import time

import pytest
from test_sequencer_tracks import (  # noqa: F401
    sequence_samples as sequence_samples_fixture,
)


def _files(esp):
    esp.key("SHIFT")
    esp.wait_state(shift=1)
    esp.softkey("Files")
    esp.wait_state(page="Pattern_Files", fileready=1)


def _new(esp):
    esp.softkey("New")
    esp.wait_state(fileconfirm=3)
    esp.softkey("Confirm")
    esp.wait_state(fileready=1, fileconfirm=0, fileerror=0, timeout=10)


@pytest.mark.both
@pytest.mark.sdcard
def test_pattern_files_preserve_hidden_steps_tempo_and_track_instruments(
    esp32, daisy, sequence_samples
):
    esp = esp32
    daisy.bind_track(1, sequence_samples[0])
    daisy.note(1, 60)
    daisy.wait_state(voices=1)
    bindings = daisy.tracks()
    esp.open_menu("Sequencer")
    esp.wait_state(seqready=1)
    _files(esp)
    _new(esp)
    esp.softkey("Back")
    esp.wait_state(page="Sequencer", seqready=1)
    esp.page("LENGTH", 64)
    esp.wait_state(seqlen=64, seqready=1)
    esp.page("FOCUS", 16, 64)
    esp.wait_state(seqtrack=16, seqpage=49, seqready=1, seqbits=0)
    esp.page("TOGGLE")
    esp.wait_state(seqbits=32768, seqready=1)
    esp.page("NOTE", 75)
    esp.wait_state(seqnote=75, seqready=1)
    esp.page("VELOCITY", 77)
    esp.wait_state(seqvel=77, seqready=1)
    esp.page("PROBABILITY", 63)
    esp.wait_state(seqprob=63, seqready=1)
    esp.key("SHIFT")
    esp.wait_state(shift=1)
    esp.softkey("Mute")
    esp.wait_state(seqmuted=1, seqready=1)
    esp.page("SWING", 63)
    esp.wait_state(seqswing=63, seqready=1)
    esp.page("TEMPO", 13700)
    esp.wait_state(seqtempo=13700, seqready=1)
    esp.page("LENGTH", 16)
    esp.wait_state(seqlen=16, seqready=1)
    esp.softkey("Play")
    esp.wait_state(seqplaying=1)
    _files(esp)
    name = "HIL pat " + str(int(time.time()))
    esp.page("NAME", name)
    daisy.msg(0x4B, struct.pack("<H", sequence_samples[0]))
    daisy.wait_state(streaming=1)
    esp.softkey("Save copy")
    esp.wait_state(
        fileready=1, fileerror=0, filename=name.replace(" ", "_"), timeout=10
    )
    daisy.wait_state(streaming=0, voices=1)
    esp.softkey("Save copy")
    esp.wait_state(fileready=1, fileerror=4, timeout=10)
    daisy.wait_state(streaming=0, voices=1)
    assert daisy.tracks() == bindings
    esp.softkey("Back")
    esp.wait_state(seqready=1, seqplaying=1)
    esp.page("TEMPO", 15100)
    esp.wait_state(seqtempo=15100, seqready=1)
    _files(esp)
    _new(esp)
    esp.page("NAME", name)
    esp.softkey("Load")
    esp.wait_state(fileconfirm=2)
    esp.softkey("Cancel")
    esp.wait_state(fileconfirm=0, fileready=1)
    daisy.msg(0x4B, struct.pack("<H", sequence_samples[0]))
    daisy.wait_state(streaming=1)
    esp.softkey("Load")
    esp.wait_state(fileconfirm=2)
    esp.softkey("Confirm")
    esp.wait_state(
        fileready=1, fileerror=0, filename=name.replace(" ", "_"), timeout=10
    )
    daisy.wait_state(streaming=0, voices=1)
    assert daisy.tracks() == bindings
    esp.page("NAME", "Missing " + str(int(time.time())))
    esp.softkey("Load")
    esp.wait_state(fileconfirm=2)
    esp.softkey("Confirm")
    esp.wait_state(fileready=1, fileerror=3, timeout=10)
    esp.softkey("Back")
    esp.wait_state(
        seqready=1,
        seqplaying=0,
        seqlen=16,
        seqswing=63,
        seqtempo=15100,
    )
    esp.page("LENGTH", 64)
    esp.wait_state(seqready=1, seqlen=64)
    esp.page("FOCUS", 16, 64)
    esp.wait_state(
        seqready=1,
        seqnote=75,
        seqvel=77,
        seqprob=63,
        seqmuted=1,
        seqbits=32768,
    )
    _files(esp)
    _new(esp)
    esp.softkey("Back")
    esp.wait_state(seqready=1, seqplaying=0, seqlen=16, seqtempo=15100)
    esp.page("TEMPO", 12000)
    esp.wait_state(seqready=1, seqtempo=12000)
    esp.home()
