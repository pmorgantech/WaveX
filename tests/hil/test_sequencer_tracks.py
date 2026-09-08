"""Track-addressed sequencer routing and sample retirement on real hardware."""

import struct
import time

import pytest


def _transport(daisy, play):
    daisy.msg(0x50, struct.pack("<BBBBHH", int(play), 0, 0, 0, 12000, 0))


def _op(daisy, op, track=0, step=0, value8=0, value16=0):
    daisy.msg(
        0x51, struct.pack("<BBBBHh", op, track, step, value8, value16, 0)
    )  # noqa: E501


def _loop(daisy, sample_id, enabled=True, end=0):
    daisy.msg(
        0x3C,
        struct.pack(
            "<HBBhIIIIHH", sample_id, enabled, 0, 0, 0, end, 0, end, 0, 0
        ),  # noqa: E501
    )


def _pattern(daisy, tracks, length=4):
    _transport(daisy, False)
    _op(daisy, 5, value16=length)
    _op(daisy, 7, value8=60)
    # Mute rows outside the fixture; reset all visible steps so previous
    # tests or bench patterns cannot add a trigger behind our assertions.
    for track in range(16):
        _op(daisy, 4, track, value8=int(track in tracks))
        for step in range(length):
            _op(daisy, 0, track, step, int(track in tracks and step == 0), 100)


@pytest.fixture
def sequence_samples(daisy, sample_path, sample_path2):
    _transport(daisy, False)
    daisy.reset_samples()
    ids = []
    for path in (sample_path, sample_path2):
        before = set(daisy.samples())
        daisy.load_sample(1000, path)
        deadline = time.monotonic() + 20
        while not (added := set(daisy.samples()) - before):
            assert time.monotonic() < deadline, f"sample did not load: {path}"
            time.sleep(0.1)
        sample_id = added.pop()
        _loop(daisy, sample_id)
        ids.append(sample_id)
    yield ids
    _transport(daisy, False)
    daisy.reset_samples()
    for track in range(16):
        _op(daisy, 4, track, value8=1)


@pytest.mark.daisy
@pytest.mark.sdcard
def test_four_sequence_rows_play_and_release_their_own_tracks(
    daisy, sequence_samples
):  # noqa: E501
    tracks = (0, 1, 2, 15)
    for track in tracks:
        daisy.bind_track(track, sequence_samples[0])
    _pattern(daisy, tracks, length=16)
    bindings = daisy.tracks()
    _transport(daisy, True)
    daisy.wait_state(voices=4)
    _transport(daisy, False)
    for remaining, track in zip((3, 2, 1, 0), reversed(tracks)):
        daisy.note(track, 60, on=False)
        daisy.wait_state(voices=remaining)
    assert daisy.tracks() == bindings


@pytest.mark.daisy
@pytest.mark.sdcard
def test_rebinding_sequence_track_preserves_another_tracks_held_note(
    daisy, sequence_samples
):
    sample_a, sample_b = sequence_samples
    daisy.bind_track(1, sample_a)
    daisy.bind_track(15, sample_a)
    _pattern(daisy, (15,))
    daisy.note(1, 60)
    daisy.wait_state(voices=1)
    _transport(daisy, True)
    daisy.wait_state(voices=lambda n: int(n) >= 2)

    daisy.unbind_track(15)
    daisy.wait_state(voices=1)
    time.sleep(
        1.1
    )  # more than two pattern cycles: retired row cannot retrigger  # noqa: E501
    assert daisy.state()["voices"] == "1"

    daisy.bind_track(15, sample_b)
    daisy.wait_state(voices=lambda n: int(n) >= 2)
    daisy.unbind_track(15)
    daisy.wait_state(voices=1)


@pytest.mark.daisy
@pytest.mark.sdcard
def test_sample_edit_refreshes_future_sequence_triggers(
    daisy, sequence_samples
):  # noqa: E501
    sample_a, sample_b = sequence_samples
    daisy.bind_track(1, sample_a)
    daisy.bind_track(15, sample_b)
    _pattern(daisy, (15,))
    daisy.note(1, 60)
    _transport(daisy, True)
    daisy.wait_state(voices=lambda n: int(n) >= 2)

    # Existing looped voices keep their snapshots until released. Future hits
    # must use the edited short one-shot without restarting the transport.
    _loop(daisy, sample_b, enabled=False, end=256)
    daisy.note(15, 60, on=False)
    time.sleep(1.1)
    daisy.wait_state(voices=1, timeout=2)


@pytest.mark.daisy
@pytest.mark.sdcard
def test_sfz_load_refreshes_sequence_track_without_stopping_other_tracks(
    daisy, sequence_samples, sfz_path
):
    daisy.bind_track(1, sequence_samples[0])
    daisy.bind_track(15, sequence_samples[1])
    _pattern(daisy, (15,))
    daisy.note(1, 60)
    _transport(daisy, True)
    daisy.wait_state(voices=lambda n: int(n) >= 2)

    daisy.msg(
        0x60,
        struct.pack(
            "<IBBH256sBBBhBB4x",
            2000,
            15,
            2,
            0,
            sfz_path.encode(),
            0,
            0,
            0,
            0,
            0,
            0,
        ),
    )
    deadline = time.monotonic() + 30
    while not daisy.tracks()[15].startswith("instrument:"):
        assert time.monotonic() < deadline, "SFZ import did not finish"
        time.sleep(0.2)
    # The newly loaded Instrument must retrigger without restarting transport.
    daisy.wait_state(voices=lambda n: int(n) >= 2)
    daisy.unbind_track(15)
    daisy.wait_state(voices=1)
    time.sleep(1.1)
    assert daisy.state()["voices"] == "1"
