"""FIFO live key releases remain independent of routing and render slots."""

import pytest
from test_sequencer_tracks import (  # noqa: F401
    sequence_samples as sequence_samples_fixture,
)


@pytest.mark.daisy
@pytest.mark.sdcard
def test_repeated_midi_keys_and_track_keys_release_independently(
    daisy, sequence_samples
):
    daisy.bind_track(0, sequence_samples[0])
    daisy.set_midi_in(0, daisy.MIDI_IN_OMNI)
    try:
        daisy.midi_note(1, 60)
        daisy.midi_note(1, 60)
        daisy.midi_note(2, 60)
        daisy.note(0, 60)
        daisy.wait_state(voices=4)
        daisy.midi_note(1, 60, on=False)
        daisy.wait_state(voices=3)
        daisy.midi_note(1, 60, on=False)
        daisy.wait_state(voices=2)
        daisy.midi_note(2, 60, on=False)
        daisy.wait_state(voices=1)
        daisy.note(0, 60, on=False)
        daisy.wait_state(voices=0)
    finally:
        daisy.midi_note(1, 60, on=False)
        daisy.midi_note(1, 60, on=False)
        daisy.midi_note(2, 60, on=False)
        daisy.note(0, 60, on=False)
        daisy.unbind_track(0)
        daisy.reset_routing()


@pytest.mark.daisy
@pytest.mark.sdcard
def test_release_follows_original_note_after_routing_changes(
    daisy, sequence_samples
):  # noqa: E501
    daisy.bind_track(0, sequence_samples[0])
    daisy.set_midi_in(0, 1)
    try:
        daisy.midi_note(1, 62)
        daisy.wait_state(voices=1)
        daisy.set_midi_in(0, daisy.MIDI_IN_OFF)
        daisy.midi_note(1, 62, on=False)
        daisy.wait_state(voices=0)
    finally:
        daisy.midi_note(1, 62, on=False)
        daisy.unbind_track(0)
        daisy.reset_routing()


@pytest.mark.daisy
@pytest.mark.sdcard
def test_old_key_release_cannot_release_replacement_binding(
    daisy, sequence_samples
):  # noqa: E501
    daisy.bind_track(0, sequence_samples[0])
    try:
        daisy.note(0, 64)
        daisy.wait_state(voices=1)
        daisy.bind_track(0, sequence_samples[1])
        daisy.wait_state(voices=0)
        daisy.note(0, 64)
        daisy.wait_state(voices=1)
        daisy.note(0, 64, on=False)
        assert daisy.state()["voices"] == "1"
        daisy.note(0, 64, on=False)
        daisy.wait_state(voices=0)
    finally:
        daisy.note(0, 64, on=False)
        daisy.note(0, 64, on=False)
        daisy.unbind_track(0)
