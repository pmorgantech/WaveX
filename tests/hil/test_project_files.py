"""HV-007: Project transactions through the real UI, UART and SD card.

Uses a fresh bench session and unique save copies; existing card files are
never overwritten. The in-memory session is reset before and after the test.
Reboot, card removal and power interruption are deliberately separate gates.
"""

import time

import pytest
from test_instrument_lfos import _lfo
from test_sequencer_tracks import _loop


def _files(esp):
    esp.open_menu("Project")
    esp.wait_state(trackready=1)
    # Center of the Project files button; ui_theme.h owns its geometry.
    esp.tap(1132, 323)
    esp.wait_state(page="Project_Files", projectready=1)


def _confirmed(esp, label, operation, error=0):
    esp.softkey(label)
    esp.wait_state(projectconfirm=operation)
    esp.softkey("Confirm")
    return esp.wait_state(
        projectready=1,
        projectpending=0,
        projectbusy=0,
        projectconfirm=0,
        projecterror=error,
        timeout=45,
    )


@pytest.fixture
def empty_project(esp32, daisy):
    _files(esp32)
    _confirmed(esp32, "New", 3)
    daisy.wait_state(voices=0, streaming=0)
    yield
    _files(esp32)
    _confirmed(esp32, "New", 3)
    esp32.home()


def _track(esp, number, **expected):
    esp.open_menu("Project")
    esp.page("SELECT", number)
    return esp.wait_state(
        trackready=1, mixready=1, track=number - 1, **expected
    )  # noqa: E501


def _pattern(esp, **expected):
    esp.open_menu("Sequencer")
    esp.wait_state(seqready=1)
    esp.page("LENGTH", 64)
    esp.wait_state(seqready=1, seqlen=64)
    esp.page("FOCUS", 16, 64)
    return esp.wait_state(seqready=1, seqtrack=16, **expected)


@pytest.mark.both
@pytest.mark.sdcard
def test_project_round_trip_and_failed_load_preserves_session(
    esp32, daisy, empty_project, sample_path
):
    esp = esp32
    name = "HIL P " + str(time.time_ns())[-16:]
    before = set(daisy.samples())
    daisy.load_sample(1000, sample_path)
    deadline = time.monotonic() + 30
    while not (added := set(daisy.samples()) - before):
        assert (
            time.monotonic() < deadline
        ), f"sample did not load: {sample_path}"  # noqa: E501
        time.sleep(0.1)
    sample = added.pop()
    _loop(daisy, sample)
    daisy.bind_track(0, sample)
    state = daisy.cmd("LFO", 0, 0)
    _lfo(
        daisy,
        890001,
        int(state["revision"]),
        0,
        (4, 8, 0, 1, 0.01, 0.125, 0.75),
    )
    lfo = daisy.cmd("LFO", 0, 0)
    assert lfo["error"] == "0" and lfo["completed"] == "890001"
    assert lfo["rate"] == "10" and lfo["sync"] == "8"

    _track(esp, 1, trackloaded=1)
    for field, value, readback in (
        ("MIDI", 3, "midiin"),
        ("LEVEL", 3900, "mixgain"),
        ("PAN", 12000, "mixpan"),
        ("MUTE", 1, "mixmute"),
    ):
        esp.page(field, value)
        esp.wait_state(trackready=1, mixready=1, **{readback: value})
    esp.open_menu("Mixer")
    esp.page("SELECT", 0)
    esp.wait_state(mixready=1, mixtrack=0)
    esp.page("LEVEL", 3600)
    esp.wait_state(mixready=1, mixgain=3600)

    _pattern(esp, seqbits=0)
    for field, value, readback in (
        ("TOGGLE", None, "seqbits"),
        ("NOTE", 75, "seqnote"),
        ("VELOCITY", 77, "seqvel"),
        ("PROBABILITY", 63, "seqprob"),
        ("SWING", 63, "seqswing"),
        ("TEMPO", 13700, "seqtempo"),
        ("LENGTH", 16, "seqlen"),
    ):
        esp.page(field, *(() if value is None else (value,)))
        esp.wait_state(
            seqready=1, **{readback: 32768 if value is None else value}
        )  # noqa: E501

    _files(esp)
    esp.page("NAME", name)
    esp.softkey("Save copy")
    esp.wait_state(
        projectready=1,
        projecterror=0,
        projectname=name.replace(" ", "_"),
        timeout=45,
    )
    esp.softkey("Save copy")
    esp.wait_state(projectready=1, projecterror=4, timeout=45)

    for label, operation in (("New", 3), ("Load", 2)):
        esp.softkey(label)
        esp.wait_state(projectconfirm=operation)
        esp.softkey("Cancel")
        esp.wait_state(projectready=1, projectconfirm=0)
    assert daisy.routing()[0] == 3
    assert daisy.cmd("LFO", 0, 0)["rate"] == "10"

    _confirmed(esp, "New", 3)
    assert all(value == "empty" for value in daisy.tracks().values())
    _track(esp, 1, trackloaded=0, midiin=1, mixmute=0)
    _files(esp)
    esp.page("NAME", name)
    _confirmed(esp, "Load", 2)
    _track(esp, 1, trackloaded=1, mixgain=3900, mixpan=12000, mixmute=1)  # noqa: E501
    restored = daisy.cmd("LFO", 0, 0)
    for key in (
        "wave",
        "sync",
        "retrigger",
        "follow",
        "rate",
        "delay",
        "fade",
    ):  # noqa: E501
        assert restored[key] == lfo[key], (key, restored, lfo)
    esp.open_menu("Mixer")
    esp.page("SELECT", 0)
    esp.wait_state(mixready=1, mixtrack=0, mixgain=3600, mixsolo=0)
    esp.open_menu("Sequencer")
    esp.wait_state(
        seqready=1, seqlen=16, seqswing=63, seqtempo=13700, seqplaying=0
    )  # noqa: E501
    _pattern(esp, seqbits=32768, seqnote=75, seqvel=77, seqprob=63)

    bindings = daisy.tracks()
    _files(esp)
    esp.page("NAME", "Missing " + str(time.time_ns())[-14:])
    _confirmed(esp, "Load", 2, error=3)
    assert daisy.tracks() == bindings
    _track(
        esp, 1, trackloaded=1, midiin=3, mixgain=3900, mixpan=12000, mixmute=1
    )  # noqa: E501
    _pattern(esp, seqbits=32768, seqnote=75, seqvel=77, seqprob=63)
