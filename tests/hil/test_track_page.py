"""Touch Track selection and authoritative MIDI routing."""

import time

import pytest
from test_sequencer_tracks import (  # noqa: F401
    sequence_samples as sequence_samples_fixture,
)


@pytest.mark.both
@pytest.mark.sdcard
def test_track_page_reads_routing_and_preserves_other_tracks(
    esp32, daisy, sequence_samples
):
    original = daisy.routing()
    daisy.bind_track(0, sequence_samples[0])
    daisy.bind_track(15, sequence_samples[1])
    bindings = daisy.tracks()
    try:
        daisy.set_midi_in(0, 1)
        daisy.set_midi_in(15, 16)
        esp32.open_menu("Track")
        esp32.page("SELECT", 16)
        esp32.wait_state(
            trackready=1,
            track=15,
            trackfirst=9,
            trackloaded=1,
            midiin=16,
        )
        esp32.page("MIDI", 255)
        esp32.wait_state(trackready=1, midiin=255)
        daisy.midi_note(16, 60)
        time.sleep(0.1)
        assert daisy.state()["voices"] == "0"
        daisy.midi_note(16, 60, on=False)
        daisy.note(0, 60)
        daisy.wait_state(voices=1)
        esp32.page("MIDI", 3)
        esp32.wait_state(trackready=1, midiin=3)
        assert daisy.state()["voices"] == "1"
        daisy.midi_note(3, 60)
        daisy.wait_state(voices=2)
        daisy.midi_note(3, 60, on=False)
        daisy.wait_state(voices=1)
        esp32.page("SELECT", 1)
        esp32.wait_state(trackready=1, track=0, trackfirst=1, midiin=1)
        daisy.set_midi_in(0, 0)
        esp32.wait_state(trackready=1, midiin=0)
        esp32.page("SELECT", 16)
        esp32.wait_state(trackready=1, midiin=3)
        assert daisy.tracks() == bindings
        assert daisy.routing()[0] == 0
        esp32.page("MIDI", 255)
        esp32.wait_state(trackready=1, midiin=255)
        esp32.softkey("MIDI -")
        esp32.wait_state(trackready=1, midiin=16)
        esp32.softkey("Tracks 1-8")
        esp32.wait_state(trackfirst=1, track=15)
    finally:
        daisy.note(0, 60, on=False)
        daisy.note(15, 60, on=False)
        for track, midi_in in original.items():
            daisy.set_midi_in(track, midi_in)
        esp32.home()
