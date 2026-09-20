"""Melodic lanes, gate cleanup, recording and SD persistence on both boards."""

import struct
import time

import pytest
from test_pattern_files import _back, _files, _new
from test_project_files import empty_project as empty_project_fixture  # noqa: F401,E501
from test_sequencer_tracks import _loop, _op, _transport


def _notes(esp, step=0):
    esp.open_menu("Sequencer")
    esp.wait_state(seqready=1)
    esp.page("FOCUS", 1, step + 1)
    esp.wait_state(seqready=1, seqtrack=1)
    esp.page("NOTES")
    return esp.wait_state(page="Step_Notes", notesready=1, step=step)


def _lane(esp, lane, note, velocity=100, gate=24):
    esp.page("NOTE", lane, note, velocity, gate)
    esp.wait_state(
        notesready=1, lane=lane, note=note, velocity=velocity, gate=gate
    )  # noqa: E501


def _load(daisy, path):
    before = set(daisy.samples())
    daisy.load_sample(1000, path)
    deadline = time.monotonic() + 30
    while not (added := set(daisy.samples()) - before):
        assert time.monotonic() < deadline
        time.sleep(0.1)
    return added.pop()


@pytest.fixture
def released_keys(daisy):
    yield
    # Balance test-owned presses even when an earlier assertion fails.
    daisy.note(1, 55, on=False)
    for note in (61, 65, 68, 73, 80):
        daisy.note(0, note, on=False)


@pytest.mark.both
@pytest.mark.sdcard
def test_chords_gates_recording_and_pattern_recall(
    esp32, daisy, empty_project, sample_path, released_keys
):
    esp = esp32
    daisy.reset_samples()
    sid = _load(daisy, sample_path)
    _loop(daisy, sid)
    daisy.bind_track(0, sid)
    daisy.bind_track(1, sid)
    _op(daisy, 5, value16=32)
    _notes(esp)
    esp.page("QUANTIZE", 2)
    esp.wait_state(notesready=1, quantize=2)
    esp.key("SHIFT")
    esp.wait_state(shift=1, sk2="Quant:_Half")
    esp.softkey("Quant: Half")
    esp.wait_state(notesready=1, quantize=0)
    esp.wait_state(shift=0)
    esp.page("MELODIC", 1)
    esp.wait_state(notesready=1, melodic=1)
    for lane, note in enumerate((60, 64, 67, 72)):
        _lane(esp, lane, note, gate=0)
    daisy.note(1, 55)
    daisy.wait_state(voices=1)
    esp.softkey("Play")
    esp.wait_state(notesready=1, playing=1)
    daisy.wait_state(voices=5)
    esp.softkey("Stop")
    esp.wait_state(notesready=1, playing=0)
    daisy.wait_state(voices=1)  # live ownership survives sequence Stop
    daisy.note(1, 55, on=False)
    daisy.wait_state(voices=0)
    for lane, note in enumerate((60, 64, 67, 72)):
        _lane(esp, lane, note, gate=48)
    esp.softkey("Play")
    daisy.wait_state(voices=4)
    daisy.wait_state(voices=0, timeout=2)
    esp.wait_state(notesready=1, playing=1)
    esp.softkey("Stop")
    esp.wait_state(notesready=1, playing=0)

    # Stopped step record groups held keys and advances after the final release.  # noqa: E501
    esp.page("STEP", 4)
    esp.wait_state(notesready=1, step=4)
    esp.page("MODE", 1)
    esp.wait_state(notesready=1, record=1)
    for note in (61, 65, 68, 73):
        daisy.note(0, note)
    esp.wait_state(notesready=1, feedback=1)
    for note in (61, 65, 68, 73):
        daisy.note(0, note, on=False)
    for lane, note in enumerate((61, 65, 68, 73)):
        esp.page("LANE", lane)
        esp.wait_state(notesready=1, note=note, velocity=100, gate=24)
    esp.wait_state(feedback=0)
    esp.page("STEP", 0)
    esp.wait_state(notesready=1, step=0)
    daisy.msg(0x50, struct.pack("<BBBBHH", 1, 0, 2, 1, 12000, 0))
    daisy.note(0, 80)
    time.sleep(0.2)
    daisy.note(0, 80, on=False)
    daisy.msg(0x50, struct.pack("<BBBBHH", 0, 0, 2, 1, 12000, 0))
    esp.page("LANE", 0)
    esp.wait_state(
        notesready=1, record=2, note=80, gate=lambda n: 30 <= int(n) <= 80
    )  # noqa: E501
    daisy.msg(0x50, struct.pack("<BBBBHH", 1, 0, 3, 1, 12000, 0))
    daisy.note(0, 80)
    esp.wait_state(notesready=1, record=3, velocity=0)
    daisy.note(0, 80, on=False)
    daisy.msg(0x50, struct.pack("<BBBBHH", 0, 0, 3, 1, 12000, 0))
    esp.wait_state(notesready=1, playing=0)
    esp.page("MODE", 0)
    esp.wait_state(notesready=1, record=0)
    esp.softkey("Back")
    esp.wait_state(seqready=1)
    _files(esp)
    name = "HIL Melody " + str(time.time_ns())[-12:]
    esp.page("NAME", name)
    esp.softkey("Save copy")
    esp.wait_state(
        fileready=1, fileerror=0, filename=name.replace(" ", "_"), timeout=20
    )
    _new(esp)
    esp.page("NAME", name)
    esp.softkey("Load")
    esp.wait_state(fileconfirm=2)
    esp.softkey("Confirm")
    esp.wait_state(
        fileready=1,
        fileconfirm=0,
        fileerror=0,
        filename=name.replace(" ", "_"),
        timeout=20,
    )
    _back(esp)
    _notes(esp, 4)
    for lane, note in enumerate((61, 65, 68, 73)):
        esp.page("LANE", lane)
        esp.wait_state(
            notesready=1, melodic=1, note=note, velocity=100, gate=24
        )  # noqa: E501
    daisy.wait_state(underruns=0, dropped=0)


@pytest.mark.both
@pytest.mark.sdcard
@pytest.mark.slow
def test_four_note_progression_and_drum_ten_minute_soak(
    esp32, daisy, empty_project, sample_path, sample_path2
):
    daisy.reset_samples()
    melody = _load(daisy, sample_path)
    drum = _load(daisy, sample_path2)
    _loop(daisy, melody)
    daisy.bind_track(0, melody)
    daisy.bind_track(1, drum)
    _op(daisy, 5, value16=32)
    _op(daisy, 15, 0, value8=1)
    for step, chord in zip(
        (0, 8, 16, 24),
        (
            (60, 64, 67, 72),
            (57, 60, 64, 69),
            (53, 57, 60, 65),
            (55, 59, 62, 67),
        ),  # noqa: E501
    ):
        for lane, note in enumerate(chord):
            daisy.msg(
                0x51,
                struct.pack(
                    "<BBBBHh", 14, 0, step, lane, note | (100 << 8), 144
                ),  # noqa: E501
            )
    for step in range(0, 32, 4):
        _op(daisy, 0, 1, step, 1, 100)
    _transport(daisy, True)
    started = time.monotonic()
    try:
        while time.monotonic() - started < 600:
            state = daisy.state()
            assert state["underruns"] == state["dropped"] == "0", state
            assert int(state["voices"]) <= 8, state
            time.sleep(2)
    finally:
        _transport(daisy, False)
    daisy.wait_state(voices=0, underruns=0, dropped=0)
