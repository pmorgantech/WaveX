"""Saved sound/Track policy and Mono held keys through UI, UART and SD."""

import time

import pytest
from test_project_files import _confirmed, _files
from test_project_files import empty_project as empty_project_fixture  # noqa: F401,E501
from test_sequencer_tracks import _loop


def _poly(esp, scope, **expected):
    esp.open_menu("Project")
    esp.page("SELECT", 1)
    esp.wait_state(trackready=1, track=0)
    esp.key("SHIFT")
    esp.wait_state(shift=1)
    esp.softkey("Sound poly" if scope == 0 else "Track poly")
    return esp.wait_state(allocready=1, allocscope=scope, **expected)


@pytest.mark.both
@pytest.mark.sdcard
def test_mono_fallback_policy_undo_and_project_restore(
    esp32, daisy, empty_project, sample_path
):
    esp = esp32
    daisy.reset_samples()
    before = set(daisy.samples())
    daisy.load_sample(1000, sample_path)
    deadline = time.monotonic() + 30
    while not (added := set(daisy.samples()) - before):
        assert time.monotonic() < deadline
        time.sleep(0.1)
    sample = added.pop()
    _loop(daisy, sample)
    daisy.bind_track(0, sample)
    _poly(esp, 0, allocmode=0, alloclimit=0, allocsteal=2)
    esp.page("MODE", 1)
    esp.wait_state(allocready=1, allocmode=1, allocdirty=1)
    esp.softkey("Revert")
    esp.wait_state(allocready=1, allocmode=0, allocdirty=0)
    esp.page("MODE", 1)
    esp.wait_state(allocready=1, allocmode=1)
    esp.page("STEAL", 1)
    esp.wait_state(allocready=1, allocsteal=1)
    esp.softkey("Apply")
    esp.wait_state(allocready=1, allocdirty=0)

    daisy.note(0, 60)
    daisy.note(0, 64)
    daisy.wait_state(voices=1)
    daisy.note(0, 64, on=False)
    # Last held key returns with a new envelope, rather than falling silent.
    time.sleep(0.4)
    daisy.wait_state(voices=1)
    daisy.note(0, 60, on=False)
    daisy.wait_state(voices=0)
    daisy.note(0, 60)
    daisy.note(0, 60)
    daisy.wait_state(voices=1)
    daisy.note(0, 60, on=False)
    time.sleep(0.4)
    daisy.wait_state(voices=1)
    daisy.note(0, 60, on=False)
    daisy.wait_state(voices=0)

    _poly(esp, 1, allocinherit=1, allocmode=1)
    esp.page("MODE", 0)
    esp.wait_state(allocready=1, allocinherit=0, allocmode=0)
    esp.page("LIMIT", 2)
    esp.wait_state(allocready=1, alloclimit=2)
    esp.page("STEAL", 0)
    esp.wait_state(allocready=1, allocsteal=0)
    esp.softkey("Apply")
    esp.wait_state(allocready=1, allocdirty=0)
    for note in (60, 62, 64):
        daisy.note(0, note)
    daisy.wait_state(voices=2)
    for note in (60, 62, 64):
        daisy.note(0, note, on=False)
    daisy.wait_state(voices=0)
    esp.page("INHERIT", 1)
    esp.wait_state(allocready=1, allocinherit=1, allocmode=1)
    esp.softkey("Revert")
    esp.wait_state(allocready=1, allocinherit=0, allocmode=0, alloclimit=2)

    name = "HIL Poly " + str(time.time_ns())[-14:]
    _files(esp)
    esp.page("NAME", name)
    esp.softkey("Save copy")
    esp.wait_state(
        projectready=1,
        projecterror=0,
        projectname=name.replace(" ", "_"),
        timeout=45,  # noqa: E501
    )
    _confirmed(esp, "New", 3)
    esp.page("NAME", name)
    _confirmed(esp, "Load", 2)
    _poly(esp, 1, allocinherit=0, allocmode=0, alloclimit=2, allocsteal=0)
    _poly(esp, 0, allocmode=1, alloclimit=0, allocsteal=1)
