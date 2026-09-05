"""Track/Instrument model stage 2 on hardware: Load lands a sample on the
selected Track, replacing asks, Assign asks, and a failed load says why.

Needs a card with the WAVs `--hil-sample` / `--hil-sample2` point at
(defaults: /Drums/Kicks/bassdr01.wav and bassdr02.wav).
"""

import os
import struct

import pytest

MSG_SAMPLE_LOAD = 0x04


@pytest.fixture(autouse=True)
def leave_the_daisy_clean(daisy):
    """Each test claims Tracks and loads samples; give them back after."""
    yield
    daisy.reset_samples()


def _split(path):
    d, f = os.path.split(path)
    return (d or "/"), f


def _open_browser(esp, path):
    """Sample > Browse, listing `path`'s directory with its file selected."""
    esp.open_menu("Sample")
    esp.page("TAB", "Browse")
    esp.wait_state(tab="Browse")
    d, f = _split(path)
    esp.page("DIR", d)
    esp.wait_state(timeout=8.0, dir=d, entries=lambda n: int(n) > 1)
    esp.page("SEL", f)
    return esp.wait_state(sel=f)


def _free_track(daisy, avoid=()):
    for t, state in sorted(daisy.tracks().items()):
        if state == "empty" and t not in avoid:
            return t
    pytest.skip("no empty Track on the Daisy")


def _wait_picker(esp):
    """The picker flag flips at once; its softkeys and prompt land on the
    next UI pass (the browser defers widget work), so wait for all three."""
    return esp.wait_state(
        picker="1",
        sk0="Cancel",
        status=lambda s: "replace" in s or "onto_Track" in s,
    )


def _load_and_wait(esp, timeout=25.0):
    """Taps Load and waits for the completion status; returns the state."""
    esp.softkey("Load")
    return esp.wait_state(
        timeout=timeout,
        status=lambda s: (
            "loaded_onto_Track" in s or "Load_failed" in s or "Error" in s
        ),
    )


@pytest.mark.both
@pytest.mark.sdcard
def test_load_binds_sample_to_empty_track_and_keys_play_it(
    esp32,
    daisy,
    sample_path,
):
    track = _free_track(daisy)
    esp32.home()
    esp32.track(track)
    esp32.wait_state(track=str(track), tstate="empty")

    st = _open_browser(esp32, sample_path)
    assert st["picker"] == "0"
    st = _load_and_wait(esp32)
    assert "loaded_onto_Track_%d" % (track + 1) in st["status"], st["status"]
    sample_id = int(st["lastid"])
    assert sample_id > 0

    # The Daisy agrees, and the frontend's shared binding cache follows.
    assert daisy.tracks()[track] == f"sample:{sample_id}"
    assert sample_id in daisy.samples()
    esp32.wait_state(tstate="sample", tid=str(sample_id))

    # And a note addressed to that Track sounds: this is the four-tap
    # workflow's whole point (model doc §6.1 A).
    daisy.note(track, 60, 100)
    daisy.wait_state(voices=lambda v: int(v) > 0, timeout=2.0)
    daisy.note(track, 60, 0, on=False)
    daisy.wait_state(voices="0", timeout=5.0)


@pytest.mark.both
@pytest.mark.sdcard
def test_load_onto_occupied_track_asks_and_cancel_leaves_it(
    esp32, daisy, sample_path, sample_path2
):
    # Occupy a Track first.
    track = _free_track(daisy)
    esp32.home()
    esp32.track(track)
    _open_browser(esp32, sample_path)
    st = _load_and_wait(esp32)
    first_id = int(st["lastid"])
    assert daisy.tracks()[track] == f"sample:{first_id}"

    # Loading another sample onto it opens the picker naming what it holds.
    esp32.page("SEL", _split(sample_path2)[1])
    esp32.wait_state(sel=_split(sample_path2)[1])
    esp32.softkey("Load")
    st = _wait_picker(esp32)
    status = st["status"]
    assert "holds_sample" in status and "replace" in status, status
    keys = esp32.softkeys(st)
    assert {"Cancel", "Track -", "Track +", "Load"} <= set(keys), keys

    esp32.softkey("Cancel", st)
    st = esp32.wait_state(
        picker="0",
        sk2="Load",
        status=lambda s: "cancelled" in s,
    )
    assert (
        daisy.tracks()[track] == f"sample:{first_id}"
    ), "cancel must not touch the Track"

    # Confirming replaces it.
    esp32.softkey("Load")
    st = _wait_picker(esp32)
    esp32.softkey("Load", st)
    st = esp32.wait_state(
        timeout=25.0,
        status=lambda s: "loaded_onto_Track" in s,
    )
    second_id = int(st["lastid"])
    assert second_id != first_id
    assert daisy.tracks()[track] == f"sample:{second_id}"


@pytest.mark.both
@pytest.mark.sdcard
def test_picker_track_keys_move_target_and_report_each_tracks_state(
    esp32, daisy, sample_path
):
    track = _free_track(daisy)
    esp32.home()
    esp32.track(track)
    _open_browser(esp32, sample_path)
    _load_and_wait(esp32)
    esp32.softkey("Load")
    st = _wait_picker(esp32)
    assert st["target"] == str(track)
    esp32.softkey("Track +", st)
    st = esp32.wait_state(
        target=str((track + 1) % 16),
        status=lambda s: "Track_%d" % ((track + 1) % 16 + 1) in s,
    )
    # The next Track is whatever the Daisy says it is; the prompt must agree.
    nxt = daisy.tracks()[(track + 1) % 16]
    if nxt == "empty":
        assert st["status"].startswith(
            "Load_sample_onto_Track_%d" % ((track + 1) % 16 + 1)
        ), st["status"]
    else:
        assert "holds" in st["status"], st["status"]
    esp32.softkey("Cancel", st)
    esp32.wait_state(picker="0")


@pytest.mark.both
@pytest.mark.sdcard
def test_sample_manager_assign_asks_before_replacing(
    esp32, daisy, sample_path, sample_path2
):
    track = _free_track(daisy)
    other_track = _free_track(daisy, avoid=(track,))
    esp32.home()
    esp32.track(track)
    _open_browser(esp32, sample_path)
    st = _load_and_wait(esp32)
    first_id = int(st["lastid"])
    # A second sample resident (on another Track) so there is something to
    # assign in place of the first.
    esp32.track(other_track)
    esp32.page("SEL", _split(sample_path2)[1])
    esp32.wait_state(sel=_split(sample_path2)[1])
    _load_and_wait(esp32)
    esp32.track(track)
    esp32.wait_state(track=str(track), tstate="sample", tid=str(first_id))

    # Sample Manager: focus a row that is not the bound one and Assign.
    esp32.page("TAB", "Manage")
    st = esp32.wait_state(tab="Manage", rows=lambda n: int(n) >= 1)
    rows = int(st["rows"])
    other = None
    for _ in range(rows):
        st = esp32.state()
        if int(st["focusid"]) not in (0, first_id):
            other = int(st["focusid"])
            break
        esp32.softkey("Down", st)
    if other is None:
        pytest.skip(
            "only the bound sample is resident; nothing to replace with",
        )

    esp32.softkey("Assign")
    st = esp32.wait_state(status=lambda s: "press_Assign_again" in s)
    assert (
        daisy.tracks()[track] == f"sample:{first_id}"
    ), "the first press must only ask"
    esp32.softkey("Assign")
    esp32.wait_state(tstate="sample", tid=str(other), timeout=5.0)
    assert daisy.tracks()[track] == f"sample:{other}"


@pytest.mark.both
@pytest.mark.sdcard
def test_loading_the_same_file_twice_is_one_pool_entry(
    esp32,
    daisy,
    sample_path,
):
    """The Pool is refcounted by path (model doc §4): a second Load of a
    resident file is a hit that answers with the id it already had, reads
    nothing from the card, and leaves one entry resident."""
    track = _free_track(daisy)
    other = _free_track(daisy, avoid=(track,))
    esp32.home()
    esp32.track(track)
    _open_browser(esp32, sample_path)
    st = _load_and_wait(esp32)
    first_id = int(st["lastid"])
    assert first_id >= 1024, "Pool ids carry a generation above slot bits"
    assert daisy.samples() == [first_id]

    esp32.track(other)
    st = _load_and_wait(esp32, timeout=5.0)
    assert int(st["lastid"]) == first_id
    assert daisy.samples() == [first_id]
    tracks = daisy.tracks()
    assert tracks[track] == f"sample:{first_id}"
    assert tracks[other] == f"sample:{first_id}"


@pytest.mark.both
def test_failed_load_reports_the_daisys_reason(esp32, daisy):
    # Drive the Daisy directly with a load for a file that is not there and
    # watch the reason cross the link to the browser's status line.
    esp32.home()
    esp32.open_menu("Sample")
    esp32.page("TAB", "Browse")
    esp32.wait_state(tab="Browse")
    path = b"/no/such/file.wav"
    payload = struct.pack("<HIHBB", 4242, 0, 0, 0, 0) + path.ljust(96, b"\0")
    daisy.msg(MSG_SAMPLE_LOAD, payload)
    st = esp32.wait_state(timeout=10.0, status=lambda s: "Load_failed" in s)
    assert "could_not_open" in st["status"], st["status"]


@pytest.mark.daisy
def test_daisy_track_bindings_are_reported_per_track(daisy):
    tracks = daisy.tracks()
    assert len(tracks) == 16
    for state in tracks.values():
        assert state == "empty" or state.startswith(
            ("sample:", "instrument:", "loading:")
        ), state
