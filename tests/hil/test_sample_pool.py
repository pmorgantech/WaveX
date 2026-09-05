"""The Sample Pool on hardware (track-and-patch-model.md §4): an import's
samples are Pool entries, shared by path, released per Track.

Needs an .sfz on the card (`--hil-sfz`, default the Minimoog
Saw_Synth_Bass.sfz) whose WAVs are small enough to load twice over.
"""

import os
import time

import pytest
from test_load_to_track import _free_track, _open_browser, _wait_picker


@pytest.fixture(autouse=True)
def leave_the_daisy_clean(daisy):
    yield
    daisy.reset_samples()


def _wait_tracks(daisy, want, timeout=30.0):
    """Polls TRACKS until each {track: predicate-or-value} in `want` holds."""
    deadline = time.monotonic() + timeout
    while True:
        tracks = daisy.tracks()
        ok = all(
            (pred(tracks[t]) if callable(pred) else tracks[t] == pred)
            for t, pred in want.items()
        )
        if ok:
            return tracks
        if time.monotonic() > deadline:
            raise AssertionError(f"tracks never met {want}; last {tracks}")
        time.sleep(0.2)


def _load_instrument(esp32, daisy, sfz_path, track):
    """Browse > Load an .sfz onto `track` through the picker; returns the
    Track's reported state once the import has committed."""
    esp32.home()
    esp32.track(track)
    _open_browser(esp32, sfz_path)
    # Load is disabled until the probe has inspected the referenced WAVs.
    esp32.wait_state(timeout=20.0, sk2="Load", sk2en="1")
    esp32.softkey("Load")
    st = _wait_picker(esp32)
    assert st["target"] == str(track)
    esp32.softkey("Load", st)
    tracks = _wait_tracks(
        daisy,
        {track: lambda s: s.startswith("instrument:")},
    )
    return tracks[track]


@pytest.mark.both
@pytest.mark.sdcard
def test_import_puts_its_samples_in_the_pool_and_plays(esp32, daisy, sfz_path):
    track = _free_track(daisy)
    name = os.path.splitext(os.path.basename(sfz_path))[0]
    state = _load_instrument(esp32, daisy, sfz_path, track)
    assert name[:12] in state, state
    samples = daisy.samples()
    assert samples, "an import's samples are Pool entries now"
    esp32.wait_state(tstate="instrument")

    daisy.note(track, 60, 100)
    daisy.wait_state(voices=lambda v: int(v) > 0, timeout=2.0)
    daisy.note(track, 60, 0, on=False)
    daisy.wait_state(voices="0", timeout=5.0)


@pytest.mark.both
@pytest.mark.sdcard
def test_two_imports_of_one_file_set_share_the_pool(esp32, daisy, sfz_path):
    a = _free_track(daisy)
    b = _free_track(daisy, avoid=(a,))
    _load_instrument(esp32, daisy, sfz_path, a)
    first = sorted(daisy.samples())
    _load_instrument(esp32, daisy, sfz_path, b)
    assert sorted(daisy.samples()) == first, "the second import is all hits"
    tracks = daisy.tracks()
    assert tracks[a].startswith("instrument:")
    assert tracks[b].startswith("instrument:")

    # Releasing one Track frees nothing the other still holds.
    daisy.unbind_track(a)
    _wait_tracks(daisy, {a: "empty"}, timeout=5.0)
    assert sorted(daisy.samples()) == first
    # Releasing the other frees the import's samples: nobody pinned them.
    daisy.unbind_track(b)
    _wait_tracks(daisy, {b: "empty"}, timeout=5.0)
    assert daisy.samples() == []


@pytest.mark.both
@pytest.mark.sdcard
def test_sample_replaces_an_import_and_frees_what_only_it_held(
    esp32, daisy, sfz_path, sample_path
):
    track = _free_track(daisy)
    _load_instrument(esp32, daisy, sfz_path, track)
    import_samples = set(daisy.samples())
    assert import_samples

    # Load a bare sample onto that Track through the browser: the picker
    # names the Instrument and asks; confirming replaces it.
    _open_browser(esp32, sample_path)
    esp32.softkey("Load")
    st = _wait_picker(esp32)
    assert "holds_Instrument" in st["status"], st["status"]
    esp32.softkey("Load", st)
    st = esp32.wait_state(
        timeout=25.0,
        status=lambda s: "loaded_onto_Track" in s or "Load_failed" in s,
    )
    assert "loaded_onto_Track" in st["status"], st["status"]
    sample_id = int(st["lastid"])
    assert daisy.tracks()[track] == f"sample:{sample_id}"
    remaining = set(daisy.samples())
    assert sample_id in remaining
    assert not (import_samples & remaining), "the import's samples are freed"


@pytest.mark.both
@pytest.mark.sdcard
def test_sample_manager_lists_an_imports_samples(esp32, daisy, sfz_path):
    """Before the Pool an import's samples were invisible to every page."""
    track = _free_track(daisy)
    _load_instrument(esp32, daisy, sfz_path, track)
    n = len(daisy.samples())
    assert n >= 1
    esp32.page("TAB", "Manage")
    st = esp32.wait_state(tab="Manage", rows=lambda r: int(r) >= min(n, 8))
    assert int(st["rows"]) >= 1
    # And releasing the Track empties the Pool: nobody pinned them.
    daisy.unbind_track(track)
    _wait_tracks(daisy, {track: "empty"}, timeout=5.0)
    assert daisy.samples() == []


@pytest.mark.both
@pytest.mark.sdcard
def test_sample_manager_pages_through_a_pool_larger_than_one_page(
    esp32,
    daisy,
):
    """The list is a window on the Pool, one frame per page (§4)."""
    import struct

    kicks = [f"/Drums/Kicks/bassdr0{i}.wav" for i in range(1, 8)]
    kicks += ["/Drums/Kicks/bdfx10.wav", "/Drums/Kicks/bdfx18.wav"]
    for i, path in enumerate(kicks):
        payload = struct.pack("<HIHBB", 5000 + i, 0, 0, 0, 0)
        payload += path.encode().ljust(96, b"\0")
        daisy.msg(daisy.MSG_SAMPLE_LOAD, payload)
    deadline = time.monotonic() + 30
    while len(daisy.samples()) < len(kicks):
        assert time.monotonic() < deadline, daisy.samples()
        time.sleep(0.3)
    ids = daisy.samples()
    daisy.unbind_track(0)
    daisy.msg(daisy.MSG_SAMPLE_SELECT, struct.pack("<HBB", ids[0], 0, 0))

    esp32.home()
    esp32.track(0)
    esp32.open_menu("Sample")
    st = esp32.wait_state(
        tab="Manage",
        total=str(len(kicks)),
        rows="8",
        first="0",
    )
    assert int(st["focusused"]) & 1, "Track 1 holds the focused sample"
    assert st["focuspinned"] == "1", "a user (MSG_SAMPLE_LOAD) load is pinned"
    for _ in range(8):
        esp32.softkey("Down")
    st = esp32.wait_state(first="8", rows="1", timeout=5.0)
    esp32.softkey("Down")
    esp32.wait_state(first="0", rows="8", timeout=5.0)
