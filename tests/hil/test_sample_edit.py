"""Sample Edit audition ownership and loop behavior on both boards."""

import struct
import time

import pytest
from test_load_to_track import _open_browser
from wavex_target import TargetError


def _wait_stream(daisy, timeout=5.0, **want):
    deadline = time.monotonic() + timeout
    while True:
        state = daisy.cmd("STREAM")
        if all(
            (
                predicate(state.get(key, ""))
                if callable(predicate)
                else state.get(key) == str(predicate)
            )
            for key, predicate in want.items()
        ):
            return state
        if time.monotonic() >= deadline:
            raise TargetError(f"stream never met {want}; last {state}")
        time.sleep(0.1)


def _edit(daisy, sample_id, start=256, end=4096, loop=True, gain=-60):
    # SampleEditMessage in protocol.h; frame positions are absolute.
    return daisy.msg(
        0x3C,
        struct.pack(
            "<HBBhIIIIHH",
            sample_id,
            loop,
            0,
            gain,
            start,
            end,
            512,
            2048,
            0,
            0,
        ),
    )


@pytest.fixture
def edited_samples(at_home, daisy, sample_path, sample_path2):
    esp = at_home
    ids = []
    for track, path in ((14, sample_path), (15, sample_path2)):
        esp.track(track)
        _open_browser(esp, path)
        esp.softkey("Load")
        st = esp.wait_state(
            timeout=20.0,
            status=lambda s: "loaded_onto_Track" in s
            or "replace" in s
            or "onto_Track" in s,
        )
        if st.get("picker") == "1":
            from test_load_to_track import _wait_picker

            esp.softkey("Load", _wait_picker(esp))
            st = esp.wait_state(
                timeout=25.0, status=lambda s: "loaded_onto_Track" in s
            )  # noqa: E501
        ids.append(int(st["lastid"]))
    yield esp, ids
    daisy.msg(0x33, bytes(4))
    daisy.reset_samples()


@pytest.mark.both
@pytest.mark.sdcard
def test_edit_does_not_change_an_unrelated_stream(edited_samples, daisy):
    _, (sample_a, sample_b) = edited_samples
    _edit(daisy, sample_b, gain=-120)
    daisy.msg(0x4B, struct.pack("<H", sample_b))
    before = _wait_stream(daisy, id=sample_b, loop=1)
    _edit(daisy, sample_a, start=1024, end=3072, loop=False, gain=0)
    after = _wait_stream(daisy, id=sample_b)
    for field in ("start", "end", "loop", "ls", "le", "gain"):
        assert after[field] == before[field], (field, before, after)

    # A fresh open adopts A's metadata, including unity gain, not B's gain.
    daisy.msg(0x4B, struct.pack("<H", sample_a))
    _wait_stream(daisy, id=sample_a, start=1024, end=3072, loop=0, gain=32767)
    before = daisy.cmd("STREAM")
    daisy.msg(0x4B, struct.pack("<H", 0))
    assert daisy.cmd("STREAM")["id"] == before["id"]


@pytest.mark.both
@pytest.mark.sdcard
def test_edit_audition_preserves_tracks_and_loops(edited_samples, daisy):
    esp, (_, sample_b) = edited_samples
    _edit(daisy, sample_b)
    bindings = daisy.tracks()

    # The last Browser load selects B for Edit. Exercise the actual softkey.
    esp.page("TAB", "Edit")
    esp.wait_state(tab="Edit", sk1="Audition")
    esp.softkey("Audition")
    stream = _wait_stream(daisy, id=sample_b, open=1, loop=1, ls=512, le=2048)
    _wait_stream(daisy, rewinds=lambda n: int(n) > int(stream["rewinds"]) + 2)
    assert daisy.tracks() == bindings
    esp.softkey("Stop", esp.wait_state(sk1="Stop"))
    _wait_stream(daisy, open=0)
    assert daisy.tracks() == bindings

    # The RAM voice inherits the same loop. A 2048-frame loop must remain
    # active well past the short kick's natural end until the note is released.
    daisy.note(15, 60, 100)
    daisy.wait_state(voices=lambda n: int(n) > 0)
    time.sleep(1.0)
    assert int(daisy.state()["voices"]) > 0
    daisy.note(15, 60, on=False)
    daisy.wait_state(voices="0")


@pytest.mark.both
@pytest.mark.sdcard
def test_pages_can_exit_repeatedly_during_waveform_traffic(
    edited_samples, daisy
):  # noqa: E501
    esp, _ = edited_samples
    for _ in range(12):
        esp.page("TAB", "Edit")
        esp.wait_state(tab="Edit", sk1="Audition")
        esp.key("SOFT2")  # queued audition can coincide with a root jump
        esp.key("PLAY")
        esp.wait_state(page="Play", depth="2")
        esp.open_menu("Sample")
        esp.page("TAB", "Browse")
        esp.wait_state(tab="Browse", sk2="Load")
    assert esp.cmd("PING") == {}
    assert daisy.cmd("PING") == {}
