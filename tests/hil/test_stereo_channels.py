"""Stereo/Mono allocation and Project mixing on the two-board bench.

Requires a resident-compatible PCM16 stereo WAV on the card. The default is
an existing bench asset; override with WAVEX_HIL_STEREO. These tests inspect
codec-bound digital meters, not an external recording or acoustic output.
"""

import itertools
import os
import re
import struct
import time
from pathlib import Path

import pytest
from test_load_to_track import _load_and_wait, _open_browser
from test_sequencer_solo import _peak
from test_sequencer_tracks import _transport

REQUESTS = itertools.count(int(time.time() * 1000) & 0x7FFFFFFF)
STEREO = os.environ.get("WAVEX_HIL_STEREO", "/Drums/Loops/loop15_3.wav.wav")


def _mono(daisy, track, enabled):
    state = daisy.cmd("OSC", track, 0)
    request = next(REQUESTS)
    daisy.msg(
        0x6A,
        struct.pack(
            "<IIBBBBffbbBB",
            request,
            int(state["revision"]),
            track,
            0,
            1,
            0,
            1.0,
            0.0,
            0,
            0,
            1,
            int(enabled),
        ),
    )
    state = daisy.cmd("OSC", track, 0)
    assert state["completed"] == str(request) and state["error"] == "0", state


def _project_mix(esp, command, value):
    """Serialize edits against backend-confirmed Project mixer state.

    PAGE acknowledges submission; mixready becomes true only after readback.
    Never replay a mutation after a missing acknowledgement.
    """
    field = {"LEVEL": "mixgain", "PAN": "mixpan", "MUTE": "mixmute"}[command]
    esp.wait_state(page="Project", mixready=1)
    esp.page(command, value)
    return esp.wait_state(page="Project", mixready=1, **{field: value})


def _release(daisy):
    for track in range(16):
        daisy.note(track, 60, on=False)
    daisy.wait_state(voices=0)


@pytest.fixture(scope="module")
def stereo_sample(esp32, daisy):
    esp32.home()
    _transport(daisy, False)
    _release(daisy)
    daisy.reset_samples()
    # Replacing a Sample preserves sound settings. Start with fresh
    # Instruments so a previous bench run's pan LFO cannot taint Mono meters.
    for track in range(16):
        request = next(REQUESTS)
        daisy.msg(
            0x60,
            struct.pack(
                "<IBBH256sBBBhBBBBH",
                request,
                track,
                9,
                0,
                b"HIL channels",
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
            ),
        )
        deadline = time.monotonic() + 15
        while True:
            state = daisy.cmd("OSC", track, 0)
            if state["completed"] == str(request) and state["busy"] == "0":
                assert state["error"] == "0", state
                break
            assert time.monotonic() < deadline, state
            time.sleep(0.05)
        daisy.unbind_track(track)
        daisy.msg(0x78, struct.pack("<BBH", 1, track, 4200))
        daisy.msg(0x78, struct.pack("<BBH", 2, track, 32768))
    daisy.msg(0x78, struct.pack("<BBH", 5, 0, 0))
    daisy.msg(0x78, struct.pack("<BBH", 8, 0, 0))
    esp32.track(0)
    _open_browser(esp32, STEREO)
    # The stream-open line reports geometry parsed by the backend, rather
    # than trusting a filename or counting voices to infer stereo.
    log = Path(daisy.logfile)
    offset = log.stat().st_size
    esp32.softkey("Audition")
    daisy.wait_state(streaming=1)
    with log.open("rb") as fh:
        fh.seek(offset)
        opened = fh.read().decode(errors="replace")
    assert re.search(r"WAV open: .* ch=2 bits=16 ", opened), opened
    esp32.softkey("Stop")
    daisy.wait_state(streaming=0)
    loaded = _load_and_wait(esp32, timeout=40)
    assert "loaded_onto_Track" in loaded["status"], loaded
    sid = int(loaded["lastid"])
    meta = daisy.cmd("SAMPLE", sid)
    # A bounded one-second region avoids relying on a song-length sustain.
    rate = int(meta["rate"])
    start = min(10 * rate, max(0, int(meta["frames"]) - rate))
    end = min(start + rate, int(meta["frames"]))
    daisy.msg(
        0x3C,
        struct.pack(
            "<HBBhIIIIHHx", sid, 1, 0, 0, start, end, start, end, 0, 0
        ),  # noqa: E501
    )
    # Keep the fixture resident across WXI replacement when selected alone.
    daisy.bind_track(15, sid)
    yield sid
    _release(daisy)
    daisy.reset_samples()
    esp32.home()


@pytest.fixture(autouse=True)
def released_notes(daisy):
    yield
    _release(daisy)


@pytest.mark.both
@pytest.mark.sdcard
@pytest.mark.parametrize("stereo_count", range(5))
def test_mixed_channel_budget_steals_a_whole_note(
    daisy,
    stereo_sample,
    stereo_count,
):
    mono_count = 8 - 2 * stereo_count
    voices = stereo_count + mono_count
    _release(daisy)
    for track in range(voices + 1):
        daisy.bind_track(track, stereo_sample)
        # The extra note costs the same as the oldest note it must steal.
        mono = track >= stereo_count if track < voices else stereo_count == 0
        _mono(daisy, track, mono)
        daisy.msg(0x78, struct.pack("<BBH", 1, track, 4200))
    before = daisy.state()
    for track in range(voices):
        daisy.note(track, 60)
        daisy.wait_state(voices=track + 1)
    daisy.note(voices, 60)
    daisy.wait_state(voices=voices)
    daisy.note(0, 60, on=False)  # Oldest note was stolen as a whole.
    assert daisy.state()["voices"] == str(voices)
    daisy.note(1, 60, on=False)
    daisy.wait_state(voices=voices - 1)
    after = daisy.state()
    assert after["underruns"] == before["underruns"]
    assert after["dropped"] == before["dropped"]


@pytest.mark.both
@pytest.mark.sdcard
def test_stereo_note_steals_two_oldest_mono_notes(daisy, stereo_sample):
    for track in range(9):
        daisy.bind_track(track, stereo_sample)
        _mono(daisy, track, track < 8)
    before = daisy.state()
    for track in range(8):
        daisy.note(track, 60)
    daisy.wait_state(voices=8)
    # Six Mono notes plus one stereo note cost eight channels.
    daisy.note(8, 60)
    daisy.wait_state(voices=7)
    for track in (0, 1):
        daisy.note(track, 60, on=False)
        assert daisy.state()["voices"] == "7"
    daisy.note(2, 60, on=False)
    daisy.wait_state(voices=6)
    after = daisy.state()
    assert after["underruns"] == before["underruns"]
    assert after["dropped"] == before["dropped"]


@pytest.mark.both
@pytest.mark.sdcard
def test_mono_toggle_preserves_held_stereo_cost_until_next_note(
    esp32, daisy, stereo_sample
):
    for track in range(6):
        daisy.bind_track(track, stereo_sample)
        _mono(daisy, track, False)
    esp32.home()
    esp32.track(0)
    esp32.open_menu("Instrument")
    esp32.page("TAB", "Filter")
    esp32.wait_state(tab="Filter", editready=1, editpending=0)
    for command, field, value in (
        ("CUTOFF", "instcutoff", 43690),
        ("RES", "instres", 44564),
        ("DRIVE", "filterdrive", 1000),
    ):
        esp32.page(command, value)
        esp32.wait_state(**{field: value, "editpending": 0, "editready": 1})
    esp32.home()
    for track in range(4):
        daisy.note(track, 60)
    daisy.wait_state(voices=4)
    for track in range(6):
        esp32.home()
        esp32.track(track)
        esp32.open_menu("Instrument")
        esp32.page("TAB", "Osc")
        esp32.wait_state(oscready=1, track=track, oscmono=0)
        esp32.page("MONO", 1)
        esp32.wait_state(oscmono=1, editpending=0)
    assert daisy.state()["voices"] == "4"
    daisy.note(4, 60)  # Steals one held stereo note: 3 stereo + 1 mono.
    daisy.wait_state(voices=4)
    daisy.note(5, 60)  # Fits the one remaining channel.
    daisy.wait_state(voices=5)
    daisy.note(0, 60, on=False)
    assert daisy.state()["voices"] == "5"


@pytest.mark.both
@pytest.mark.sdcard
def test_project_pan_gain_mute_and_mono_revert_reach_held_audio(
    esp32, daisy, stereo_sample
):
    esp = esp32
    daisy.bind_track(0, stereo_sample)
    _mono(daisy, 0, False)
    esp.track(0)
    esp.open_menu("Instrument")
    esp.page("TAB", "Osc")
    esp.wait_state(oscready=1, oscmono=0)
    esp.key("SHIFT")
    esp.softkey("Apply")
    esp.wait_state(editdirty=0, shift=0)
    daisy.note(0, 60)
    daisy.wait_state(voices=1)
    esp.open_menu("Project")
    esp.wait_state(mixready=1, track=0)
    original = esp.state()
    try:
        _project_mix(esp, "LEVEL", 6000)
        _project_mix(esp, "PAN", 32768)
        _project_mix(esp, "MUTE", 0)
        esp.wait_state(mixgain=6000, mixpan=32768, mixmute=0)
        assert min(_peak(daisy)) > 100
        _project_mix(esp, "PAN", 0)
        esp.wait_state(mixpan=0)
        left, right = _peak(daisy)
        assert left > 100 and right == 0
        _project_mix(esp, "PAN", 65535)
        esp.wait_state(mixpan=65535)
        left, right = _peak(daisy)
        assert left == 0 and right > 100
        _project_mix(esp, "PAN", 32768)
        _project_mix(esp, "LEVEL", 0)
        esp.wait_state(mixgain=0)
        assert _peak(daisy) == [0, 0]
        _project_mix(esp, "LEVEL", 6000)
        _project_mix(esp, "MUTE", 1)
        esp.wait_state(mixgain=6000, mixmute=1)
        assert _peak(daisy) == [0, 0]
        _project_mix(esp, "MUTE", 0)
        esp.wait_state(mixmute=0)
        assert min(_peak(daisy)) > 100
        assert daisy.state()["voices"] == "1"
        esp.open_menu("Instrument")
        esp.page("TAB", "Osc")
        esp.wait_state(oscmono=0, instgain=1000, instpan=500)
        esp.page("MONO", 1)
        esp.wait_state(oscmono=1, editpending=0)
        _release(daisy)
        daisy.note(0, 60)
        daisy.wait_state(voices=1)
        left, right = _peak(daisy)
        assert min(left, right) > 100
        assert abs(left - right) <= max(3, max(left, right) * 0.0001)
        esp.key("SHIFT")
        esp.softkey("Revert")
        esp.wait_state(oscmono=0, editdirty=0, editpending=0)
    finally:
        esp.open_menu("Project")
        _project_mix(esp, "LEVEL", int(original["mixgain"]))
        _project_mix(esp, "PAN", int(original["mixpan"]))
        _project_mix(esp, "MUTE", int(original["mixmute"]))
        esp.home()


@pytest.mark.both
@pytest.mark.sdcard
def test_mono_wxi_copy_restores_each_oscillator_setting(
    esp32,
    daisy,
    stereo_sample,
):
    from test_oscillators import _instrument, _wait_osc

    esp = esp32
    daisy.bind_track(0, stereo_sample)
    _mono(daisy, 0, False)
    esp.track(0)
    esp.open_menu("Instrument")
    esp.page("TAB", "Osc")
    esp.page("OSC", 1)
    esp.wait_state(oscready=1, oscmono=0)
    esp.page("MONO", 1)
    esp.wait_state(oscmono=1, editpending=0)
    esp.key("SHIFT")
    esp.softkey("Apply")
    esp.wait_state(editdirty=0)
    name = "HIL mono " + str(int(time.time()))
    request = next(REQUESTS)
    _instrument(daisy, request, 5, name)
    _wait_osc(daisy, 0, 0, completed=request, busy=0, error=0)
    # Console export advances the revision outside the page action path.
    esp.home()
    esp.open_menu("Instrument")
    esp.page("TAB", "Osc")
    esp.wait_state(oscready=1, oscmono=1)
    esp.page("MONO", 0)
    esp.wait_state(oscmono=0, editpending=0)
    esp.key("SHIFT")
    esp.softkey("Apply")
    esp.wait_state(editdirty=0)
    esp.home()
    try:
        request = next(REQUESTS)
        _instrument(daisy, request, 2, "0:/wavex/instruments/" + name + ".wxi")
        _wait_osc(daisy, 0, 0, valid=1, busy=0, zones=1, error=0)
        esp.open_menu("Instrument")
        esp.page("TAB", "Osc")
        esp.wait_state(oscmono=1, editdirty=0)
        esp.page("OSC", 2)
        esp.wait_state(osc=2, oscready=1, oscmono=0)
        esp.page("OSC", 1)
        esp.wait_state(osc=1, oscmono=1)
        daisy.note(0, 60)
        daisy.wait_state(voices=1)
        left, right = _peak(daisy)
        assert min(left, right) > 100
        assert abs(left - right) <= max(3, max(left, right) * 0.0001)
    finally:
        _release(daisy)
        esp.home()
        # Import owns sample references. Release that Instrument before the
        # module fixture tries to unload the PCM.
        request = next(REQUESTS)
        _instrument(daisy, request, 9, "HIL empty")
        _wait_osc(daisy, 0, 0, completed=request, busy=0, error=0)


@pytest.mark.both
@pytest.mark.sdcard
def test_solo_preserves_manual_mute_edits_while_soloed(
    esp32,
    daisy,
    stereo_sample,
):
    esp = esp32
    daisy.msg(0x78, struct.pack("<BBH", 8, 0, 0))
    try:
        for track in (0, 1):
            daisy.bind_track(track, stereo_sample)
            _mono(daisy, track, False)
            daisy.msg(0x78, struct.pack("<BBH", 3, track, 0))
            daisy.note(track, 60)
        daisy.wait_state(voices=2)
        esp.track(0)
        esp.open_menu("Project")
        _project_mix(esp, "MUTE", 1)
        esp.wait_state(mixmute=1)
        esp.open_menu("Sequencer")
        esp.wait_state(seqready=1)
        esp.page("SOLO", 2)
        esp.wait_state(seqsolo=2)
        assert min(_peak(daisy)) > 100
        esp.open_menu("Project")
        esp.page("SELECT", 2)
        esp.wait_state(track=1, mixready=1)
        _project_mix(esp, "MUTE", 1)
        esp.wait_state(mixmute=1)
        assert _peak(daisy) == [0, 0]
        esp.open_menu("Sequencer")
        esp.page("SOLO", 0)
        esp.wait_state(seqsolo=0)
        assert _peak(daisy) == [0, 0]
        esp.open_menu("Project")
        for track in (1, 2):
            esp.page("SELECT", track)
            esp.wait_state(track=track - 1, mixready=1, mixmute=1)
            _project_mix(esp, "MUTE", 0)
            esp.wait_state(mixmute=0)
        assert min(_peak(daisy)) > 100
        assert daisy.state()["voices"] == "2"
        esp.home()
    finally:
        daisy.msg(0x78, struct.pack("<BBH", 8, 0, 0))
        for track in (0, 1):
            daisy.msg(0x78, struct.pack("<BBH", 3, track, 0))
        esp.home()


@pytest.mark.both
@pytest.mark.sdcard
def test_sample_channel_selection_updates_audition_and_next_note_budget(
    esp32, daisy, stereo_sample
):
    for track in range(8):
        daisy.bind_track(track, stereo_sample)
        _mono(daisy, track, False)
    esp32.home()
    esp32.open_menu("Sample")
    esp32.page("TAB", "Edit")
    esp32.wait_state(editid=stereo_sample)
    esp32.page("FOCUS", 8)
    prior = 0
    for mode in (1, 2, 3, 0):
        previous = daisy.cmd("SAMPLE", stereo_sample)
        esp32.enc(mode - prior, steps=False)
        esp32.wait_state(channelmode=mode)
        deadline = time.monotonic() + 5
        while True:
            meta = daisy.cmd("SAMPLE", stereo_sample)
            if meta["channelmode"] == str(mode):
                break
            assert time.monotonic() < deadline, meta
            time.sleep(0.05)
        assert meta["wavegen"] != previous["wavegen"]
        esp32.wait_state(wavegen=meta["wavegen"])
        esp32.softkey("Audition")
        daisy.wait_state(streaming=1)
        left, right = _peak(daisy)
        assert left > 0 and right > 0, (mode, left, right)
        if mode:
            assert abs(left - right) <= 2, (mode, left, right)
        esp32.softkey("Stop")
        daisy.wait_state(streaming=0)
        for track in range(8):
            daisy.note(track, 60)
        daisy.wait_state(voices=8 if mode else 4)
        _release(daisy)
        prior = mode
