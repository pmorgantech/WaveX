"""HV-020: build a complete kit and recall a pattern addressing all 16 pads.

Uses two existing WAVs, unique WXI/Pattern copies and the real touch/UART/SD
paths. Replaces the bench session; saved source files are never overwritten.
"""

import time

import pytest
from test_pattern_files import _back, _files, _new
from test_project_files import empty_project  # noqa: F401
from test_sequencer_tracks import (  # noqa: F401
    sequence_samples as sequence_samples_fixture,
)


@pytest.mark.both
@pytest.mark.sdcard
@pytest.mark.usefixtures("empty_project")
def test_full_kit_and_pad_pattern_round_trip(
    esp32, daisy, sequence_samples, record_property
):
    esp = esp32
    name = "HIL16 " + str(time.time_ns())[-14:]
    record_property("kit_and_pattern_name", name)
    daisy.bind_track(1, sequence_samples[0])
    daisy.note(1, 60)
    daisy.wait_state(voices=1)
    preserved = daisy.tracks()[1]
    esp.track(0)
    esp.open_menu("Instrument")
    esp.softkey("Pad Map")
    esp.wait_state(kitready=1)
    esp.softkey("New kit")
    esp.wait_state(kitview=3)
    esp.softkey("Confirm")
    esp.wait_state(kitview=1)
    esp.page("NAME", name)
    esp.softkey("Confirm")
    esp.wait_state(
        kitready=1,
        kiteditable=1,
        kitview=0,
        kitname=name.replace(" ", "_"),
    )
    for pad in range(1, 17):
        esp.page("PAD", pad)
        esp.wait_state(kitready=1, kitpad=pad)
        esp.softkey("Assign")
        esp.wait_state(kitview=2, kitpick0=sequence_samples[0])
        choice = (pad - 1) % 2
        esp.page("CHOOSE", choice + 1)
        esp.wait_state(kitready=1, kitsample=sequence_samples[choice])
        if pad % 2 == 0:
            esp.softkey("Choke +")
            esp.wait_state(kitready=1, kitchoke=1)
    assert daisy.tracks()[1] == preserved
    daisy.wait_state(voices=1)
    esp.softkey("Save copy")
    esp.wait_state(kitview=1)
    esp.softkey("Confirm")
    esp.wait_state(kitready=1, kitview=0, kiterror=0, timeout=20)

    esp.track(2)
    esp.open_menu("Project")
    esp.softkey("Assign")
    browser = esp.wait_state(page="Instrument_Browser")
    # Re-entry can already restore Saved. A redundant touch is asynchronous:
    # its later refresh would invalidate a selection made from the old list.
    if browser.get("dir") != "/wavex/instruments":
        esp.softkey("Saved")
    esp.wait_state(dir="/wavex/instruments", entries=lambda n: int(n) > 1)
    selected = esp.select_file(name + ".wxi")
    record_property("saved_kit_directory_index", selected["index"])
    esp.wait_state(
        sel=(name + ".wxi").replace(" ", "_"),
        sk2="Load",
        sk2en=1,
        timeout=15,
    )
    esp.softkey("Load")
    esp.wait_state(picker=1)
    esp.softkey("Load")
    esp.wait_state(status="Instrument_loaded", timeout=20)
    esp.open_menu("Instrument")
    esp.softkey("Pad Map")
    esp.wait_state(kitready=1, kitname=name.replace(" ", "_"))
    for pad in range(1, 17):
        esp.page("PAD", pad)
        esp.wait_state(
            kitready=1,
            kitpad=pad,
            kitsample=sequence_samples[(pad - 1) % 2],
            kitchoke=int(pad % 2 == 0),
        )
    assert daisy.tracks()[1] == preserved
    bindings = daisy.tracks()

    esp.open_menu("Sequencer")
    esp.wait_state(seqready=1)
    _files(esp)
    _new(esp)
    _back(esp)
    esp.page("LENGTH", 16)
    esp.wait_state(seqready=1, seqlen=16)
    esp.page("TEMPO", 12000)
    esp.wait_state(seqready=1, seqtempo=12000)
    for step in range(1, 17):
        esp.page("FOCUS", 3, step)
        esp.wait_state(seqready=1, seqtrack=3)
        esp.page("NOTE", 59 + step)
        esp.wait_state(seqready=1, seqnote=59 + step)
        esp.page("TOGGLE")
        esp.wait_state(seqready=1, seqbits=(1 << step) - 1)
    _files(esp)
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
    esp.wait_state(fileready=1, fileconfirm=0, fileerror=0, timeout=20)
    _back(esp)
    for step in range(1, 17):
        esp.page("FOCUS", 3, step)
        esp.wait_state(seqready=1, seqbits=65535, seqnote=59 + step)
    assert daisy.tracks() == bindings
    esp.softkey("Play")
    esp.wait_state(seqplaying=1)
    daisy.wait_state(voices=lambda value: int(value) >= 2)
    # Two full 16-step loops at 120 BPM; audible order is still a bench check.
    time.sleep(4.1)
    daisy.wait_state(
        voices=lambda value: int(value) >= 2,
        underruns=0,
        dropped=0,
    )
    esp.softkey("Stop")
    esp.wait_state(seqplaying=0)
    daisy.wait_state(underruns=0, dropped=0)
    assert daisy.tracks()[1] == preserved
    esp.home()
