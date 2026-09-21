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


def _edit(
    daisy,
    sample_id,
    start=256,
    end=4096,
    loop=True,
    gain=-60,
    crossfade=0,
):
    # SampleEditMessage in protocol.h; frame positions are absolute.
    return daisy.msg(
        0x3C,
        struct.pack(
            "<HBBhIIIIHHx",
            sample_id,
            loop,
            crossfade,
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
    try:
        for track, path in ((14, sample_path), (15, sample_path2)):
            esp.track(track)
            _open_browser(esp, path)
            esp.softkey("Load")
            st = esp.wait_state(
                timeout=120.0,
                status=lambda s: "loaded_onto_Track" in s
                or "replace" in s
                or "onto_Track" in s,
            )
            if st.get("picker") == "1":
                from test_load_to_track import _wait_picker

                esp.softkey("Load", _wait_picker(esp))
                st = esp.wait_state(
                    timeout=120.0, status=lambda s: "loaded_onto_Track" in s
                )  # noqa: E501
            ids.append(int(st["lastid"]))
        yield esp, ids
    finally:
        if esp.state().get("filebusy") == "1":
            esp.wait_state(timeout=600.0, filebusy="0")
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


@pytest.mark.both
@pytest.mark.sdcard
def test_audition_startup_does_not_report_underruns(edited_samples, daisy):
    _, (_, sample_id) = edited_samples
    daisy.msg(
        0x3C,
        struct.pack("<HBBhIIIIHHx", sample_id, 1, 0, 0, 0, 0, 0, 0, 3, 3),
    )
    baseline = daisy.state()["underruns"]
    for _ in range(5):
        daisy.msg(0x4B, struct.pack("<H", sample_id))
        _wait_stream(daisy, open=1, rewinds=lambda n: int(n) >= 2)
        assert daisy.state()["underruns"] == baseline
        daisy.msg(0x33, bytes(4))
        _wait_stream(daisy, open=0)


@pytest.mark.both
@pytest.mark.sdcard
def test_standalone_save_copy_then_save_survives_pool_reload(
    edited_samples, daisy, sample_path2
):
    """Only write unique copies; source WAVs and their sidecars are untouched."""  # noqa: E501
    import posixpath

    from test_load_to_track import _wait_picker

    esp, (_, sample_id) = edited_samples
    _edit(daisy, sample_id, gain=-90, crossfade=13)
    name = "HIL Edit " + time.strftime("%Y%m%d %H%M%S")
    copy_path = posixpath.join(posixpath.dirname(sample_path2), name + ".wav")
    bindings = daisy.tracks()
    esp.page("TAB", "Edit")
    esp.key("SHIFT")
    esp.softkey("Save As", esp.wait_state(shift="1", sk4="Save_As"))
    esp.wait_state(page="Save_Sample_As", fileready="1")
    esp.page("NAME", name)
    previous = esp.state()["filecompleted"]
    esp.softkey("Save copy")
    saved = esp.wait_state(
        timeout=600.0,
        filepending="0",
        filecompleted=lambda n: int(n) > 0 and n != previous,
    )
    assert saved["fileerror"] == "0", saved
    assert daisy.tracks() == bindings
    # Explicit collision retry must fail without altering the existing copy.
    esp.softkey("Save copy")
    esp.wait_state(timeout=10.0, filepending="0", fileerror="4")
    esp.home()
    daisy.reset_samples()

    def load_copy():
        esp.track(15)
        _open_browser(esp, copy_path)
        esp.softkey("Load")
        state = esp.wait_state(
            timeout=120.0, status=lambda s: "onto_Track" in s or "replace" in s
        )
        if state.get("picker") == "1":
            esp.softkey("Load", _wait_picker(esp))
            state = esp.wait_state(
                timeout=120.0, status=lambda s: "onto_Track" in s
            )  # noqa: E501
        return int(state["lastid"])

    restored = load_copy()
    assert daisy.cmd("SAMPLE", restored)["xf"] == "13"
    daisy.msg(0x4B, struct.pack("<H", restored))
    _wait_stream(
        daisy,
        id=restored,
        start=256,
        end=4096,
        loop=1,
        ls=512,
        le=2048,
        gain=lambda n: 11000 < int(n) < 12000,
    )
    _edit(daisy, restored, start=768, end=3072, gain=-120, crossfade=5)
    esp.page("TAB", "Edit")
    esp.key("SHIFT")
    esp.softkey("Save", esp.wait_state(shift="1", sk3="Save"))
    esp.wait_state(page="Save_Sample_Edits", fileready="1")
    previous = esp.state()["filecompleted"]
    esp.softkey("Save edits")
    saved = esp.wait_state(
        timeout=15.0,
        filepending="0",
        filecompleted=lambda n: int(n) > 0 and n != previous,
    )
    assert saved["fileerror"] == "0", saved
    esp.home()
    daisy.reset_samples()
    restored = load_copy()
    assert daisy.cmd("SAMPLE", restored)["xf"] == "5"
    daisy.msg(0x4B, struct.pack("<H", restored))
    _wait_stream(
        daisy,
        id=restored,
        start=768,
        end=3072,
        loop=1,
        gain=lambda n: 8000 < int(n) < 8500,
    )


@pytest.mark.both
@pytest.mark.sdcard
def test_stereo_seam_controls_and_crossfade(edited_samples, daisy):
    esp, (_, sid) = edited_samples
    _edit(daisy, sid, crossfade=0)
    esp.page("TAB", "Edit")
    esp.wait_state(editid=sid, xf=0)
    esp.page("FOCUS", 7)
    esp.enc(20)
    esp.wait_state(xf=20)
    deadline = time.monotonic() + 5
    while daisy.cmd("SAMPLE", sid).get("xf") != "20":
        assert time.monotonic() < deadline
        time.sleep(0.05)
    esp.softkey("Audition")
    stream = _wait_stream(daisy, id=sid, loop=1)
    _wait_stream(daisy, rewinds=lambda n: int(n) > int(stream["rewinds"]) + 3)
    esp.key("SHIFT")
    esp.softkey("Check Seam", esp.wait_state(sk2="Check_Seam"))
    checked = esp.wait_state(seampending=0, seamresult=lambda n: int(n) > 0)
    assert checked["seamerror"] == "0"
    previous = checked["seamresult"]
    esp.page("FOCUS", 3)
    before = daisy.cmd("SAMPLE", sid)
    esp.key("SHIFT")
    esp.softkey("Snap", esp.wait_state(sk2="Snap"))
    result = esp.wait_state(
        seampending=0,
        seamresult=lambda n: int(n) > int(previous),
    )
    assert result["seamerror"] in ("0", "3"), result
    after = daisy.cmd("SAMPLE", sid)
    assert abs(int(after["le"]) - int(before["le"])) <= 512
    assert after["ls"] == before["ls"]
    assert after["xf"] == "20"
    assert int(after["le"]) - int(after["ls"]) >= 256
    esp.softkey("Stop", esp.wait_state(sk1="Stop"))
    daisy.note(15, 60, 100)
    daisy.wait_state(voices=lambda n: int(n) > 0)
    time.sleep(1)
    daisy.note(15, 60, on=False)
    daisy.wait_state(voices=0)
    assert daisy.state()["underruns"] == "0"


@pytest.mark.both
@pytest.mark.sdcard
def test_marker_tiles_and_handles_follow_pointer(edited_samples, daisy):
    esp, (_, sid) = edited_samples
    frames = int(daisy.cmd("SAMPLE", sid)["frames"])
    assert frames >= 4096
    markers = [
        frames // 10,
        frames * 9 // 10,
        frames * 3 // 10,
        frames * 7 // 10,
    ]
    daisy.msg(
        0x3C,
        struct.pack("<HBBhIIIIHHx", sid, 1, 0, 0, *markers, 0, 0),
    )
    esp.page("TAB", "Edit")
    esp.wait_state(editid=sid, start=markers[0], end=markers[1])
    esp.page("FOCUS", 0)
    esp.key("NAV_B_PUSH")
    esp.wait_state(focus=1)
    esp.enc(-2)
    esp.wait_state(end=lambda n: int(n) < markers[1])

    def readback(key, before, direction):
        deadline = time.monotonic() + 5
        while True:
            value = int(daisy.cmd("SAMPLE", sid)[key])
            if (value - before) * direction > 0:
                return value
            assert time.monotonic() < deadline, (key, before, value)
            time.sleep(0.05)

    # Includes the fill bars, whose LVGL children previously ate drags.
    for param, key, direction in (
        (0, "start", 1),
        (1, "end", -1),
        (2, "ls", 1),
        (3, "le", -1),
    ):
        x = 171 + param * 312
        esp.tap(x, 402)
        esp.wait_state(focus=param)
        before = int(daisy.cmd("SAMPLE", sid)[key])
        try:
            esp.cmd("TOUCH", "DOWN", x, 475)
            time.sleep(0.1)
            esp.cmd("TOUCH", "MOVE", x + direction * 45, 475)
            time.sleep(0.15)
        finally:
            esp.cmd("TOUCH", "UP", x + direction * 45, 475)
        readback(key, before, direction)

    # Two movements per held handle catch seam-view switches mid-gesture.
    for key, direction, y, half_width in (
        ("start", 1, 148, 15),
        ("end", -1, 148, 15),
        ("ls", 1, 342, 17),
        ("le", -1, 342, 17),
    ):
        esp.page("FOCUS", 0)
        time.sleep(0.15)
        before = int(daisy.cmd("SAMPLE", sid)[key])
        x = 20 + before * 1240 // frames + half_width
        try:
            esp.cmd("TOUCH", "DOWN", x, y)
            time.sleep(0.1)
            esp.cmd("TOUCH", "MOVE", x + direction * 35, y)
            time.sleep(0.2)
            first = readback(key, before, direction)
            esp.cmd("TOUCH", "MOVE", x + direction * 70, y)
            time.sleep(0.2)
            readback(key, first, direction)
        finally:
            esp.cmd("TOUCH", "UP", x + direction * 70, y)
    assert daisy.state()["underruns"] == "0"
