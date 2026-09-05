"""Per-Track MIDI routing on hardware (track-and-patch-model.md §2.2).

A note arriving on a MIDI channel reaches every Track whose `midi_in`
matches; a Track-addressed note reaches exactly one. The host tests make
those claims against the routing table - here they are made against the
running engine, by counting the voices a note actually starts.

Backend-only by design. Routing is engine state, so the test drives the
Daisy console directly (`MSG_SAMPLE_LOAD`/`MSG_SAMPLE_SELECT` to get a
sounding Track, `MSG_TRACK_OP` to route it) rather than through the
frontend's browser: fewer moving parts between the claim and the check,
and no MIDI cable, since the console's `NOTE ... MIDI` form builds exactly
the NoteMessage the ESP32's MIDI task forwards.
"""

import time

import pytest

# Long enough for a note to be picked off the queue and started by the next
# callback, short enough that a test of sixteen channels stays quick.
_TRIGGER_SETTLE_S = 0.35
_RELEASE_SETTLE_S = 0.6


@pytest.fixture(autouse=True)
def leave_the_daisy_clean(daisy):
    yield
    daisy.reset_routing()
    daisy.reset_samples()


@pytest.fixture
def sounding_track(daisy, sample_path):
    """A Track bound to a resident sample, so a note on it makes a voice.

    Yields (track, sample_id). The Pool assigns the id - the one passed to
    MSG_SAMPLE_LOAD is a request, not a promise - so the binding uses what
    the Daisy reports back.
    """
    # Start from known routing: an earlier test in the session may have left
    # a Track pointed somewhere, and "its own channel" has to mean the
    # default for the assertions below to say anything.
    daisy.reset_routing()

    track = None
    for t, state in sorted(daisy.tracks().items()):
        if state == "empty":
            track = t
            break
    if track is None:
        pytest.skip("no empty Track on the Daisy")

    # Which id appeared, not simply the first resident one: another test may
    # have left samples in the Pool, and binding one of those would test
    # something other than what this file claims to.
    before = set(daisy.samples())
    daisy.load_sample(1, sample_path)
    deadline = time.monotonic() + 10.0
    while True:
        new_ids = set(daisy.samples()) - before
        if new_ids:
            sample_id = sorted(new_ids)[0]
            break
        if time.monotonic() > deadline:
            pytest.skip(f"{sample_path} never became resident")
        time.sleep(0.2)

    daisy.bind_track(track, sample_id)
    assert daisy.tracks()[track] == f"sample:{sample_id}"
    yield track, sample_id


def _voices_started(daisy, send, release):
    """Peak voice count a note starts, then releases it.

    Voices are counted rather than listened to because that is exactly the
    fan-out claim: one Track sounding is one voice, a two-Track layer two.
    """
    daisy.wait_state(voices="0", timeout=5.0)
    send()
    time.sleep(_TRIGGER_SETTLE_S)
    voices = int(daisy.state()["voices"])
    release()
    time.sleep(_RELEASE_SETTLE_S)
    return voices


def _on_midi(daisy, channel):
    return (
        lambda: daisy.midi_note(channel, 60, 100),
        lambda: daisy.midi_note(channel, 60, 0, on=False),
    )


def _on_track(daisy, track):
    return (
        lambda: daisy.note(track, 60, 100),
        lambda: daisy.note(track, 60, 0, on=False),
    )


@pytest.mark.daisy
def test_default_routing_is_one_channel_per_track(daisy):
    """Track 1 listens on MIDI 1, Track 16 on MIDI 16 - the mapping the
    engine had before Tracks had a midi_in at all."""
    assert daisy.routing() == {t: t + 1 for t in range(16)}


@pytest.mark.daisy
@pytest.mark.sdcard
def test_a_track_hears_its_own_channel_and_no_other(daisy, sounding_track):
    track, _ = sounding_track
    mine = track + 1
    theirs = 1 + (mine % 16)

    assert _voices_started(daisy, *_on_midi(daisy, mine)) == 1
    assert (
        _voices_started(daisy, *_on_midi(daisy, theirs)) == 0
    ), f"MIDI {theirs} belongs to another Track"


@pytest.mark.daisy
@pytest.mark.sdcard
def test_omni_track_hears_every_channel(daisy, sounding_track):
    track, _ = sounding_track
    daisy.set_midi_in(track, daisy.MIDI_IN_OMNI)
    assert daisy.routing()[track] == daisy.MIDI_IN_OMNI

    for channel in (1, 7, 16):
        assert (
            _voices_started(daisy, *_on_midi(daisy, channel)) == 1
        ), f"an Omni Track must hear MIDI {channel}"


@pytest.mark.daisy
@pytest.mark.sdcard
def test_off_track_ignores_midi_but_still_takes_track_addressed_notes(
    daisy, sounding_track
):
    """A sequencer-only Track: deaf to the keyboard, still playable by the
    sequencer. The removed global filter could not express this - it gated
    every Track at once, and it dropped only note-ons."""
    track, _ = sounding_track
    channel = track + 1

    daisy.set_midi_in(track, daisy.MIDI_IN_OFF)
    assert daisy.routing()[track] == daisy.MIDI_IN_OFF

    assert (
        _voices_started(daisy, *_on_midi(daisy, channel)) == 0
    ), "an Off Track must not hear its old channel"
    assert (
        _voices_started(daisy, *_on_track(daisy, track)) == 1
    ), "an Off Track must still take a Track-addressed note"


@pytest.mark.daisy
@pytest.mark.sdcard
def test_moving_a_track_to_another_channel_takes_effect_both_ways(
    daisy, sounding_track
):
    """Routing is a setting, not a filter: the Track answers where it is
    pointed and stops answering where it was."""
    track, _ = sounding_track
    was = track + 1
    now = 1 + (was % 16)

    daisy.set_midi_in(track, now)
    assert daisy.routing()[track] == now

    assert (
        _voices_started(daisy, *_on_midi(daisy, now)) == 1
    ), f"the Track must now answer MIDI {now}"
    assert (
        _voices_started(daisy, *_on_midi(daisy, was)) == 0
    ), f"the Track must no longer answer MIDI {was}"


@pytest.mark.daisy
def test_track_op_rejects_an_out_of_range_value(daisy):
    """A bad midi_in is refused, not clamped onto something plausible: the
    two ends disagreeing about the model should be visible, not absorbed."""
    before = daisy.routing()

    daisy.set_midi_in(0, 17)  # 1..16 and 0xFF are the only legal values
    assert daisy.routing() == before

    daisy.set_midi_in(99, daisy.MIDI_IN_OMNI)  # no such Track
    assert daisy.routing() == before
