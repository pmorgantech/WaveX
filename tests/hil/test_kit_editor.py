"""Touch kit creation, sparse pad assignment and SD save/reload."""

import struct
import time

import pytest
from test_sequencer_tracks import (  # noqa: F401
    sequence_samples as sequence_samples_fixture,
)


@pytest.mark.both
@pytest.mark.sdcard
def test_touch_kit_save_reload_preserves_samples_and_other_tracks(
    esp32, daisy, sequence_samples
):
    esp = esp32
    a, _ = sequence_samples
    daisy.bind_track(1, a)
    daisy.note(1, 60)
    daisy.wait_state(voices=1)
    esp.track(0)
    esp.open_menu("Instrument")
    esp.softkey("Pad Map")
    esp.wait_state(page="Pad_Map", kitready=1)
    esp.softkey("New kit")
    esp.wait_state(kitview=3)
    esp.softkey("Confirm")
    esp.wait_state(kitview=1)
    name = "HIL kit " + str(int(time.time()))
    esp.page("NAME", name)
    esp.softkey("Confirm")
    esp.wait_state(kitready=1, kiteditable=1, kitname=name.replace(" ", "_"))
    assert daisy.state()["voices"] == "1"
    esp.page("PAD", 16)
    esp.softkey("Assign")
    esp.wait_state(kitview=2, kitpick0=a)
    esp.page("CHOOSE", 1)
    esp.wait_state(kitready=1, kitpad=16, kitsample=a)
    esp.softkey("Choke +")
    esp.wait_state(kitready=1, kitchoke=1)
    esp.key("SHIFT")
    esp.wait_state(shift=1)
    esp.softkey("Sound")
    esp.wait_state(
        page="Pad_Sound", soundready=1, soundvalid=1, soundpad=16, soundown=0
    )
    for field, value in [
        ("CUTOFF", 1200),
        ("ATTACK", 7),
        ("DECAY", 250),
        ("SUSTAIN", 350),
    ]:
        esp.page(field, value)
        esp.wait_state(soundready=1, soundown=1, **{field.lower(): value})
    assert daisy.state()["voices"] == "1"
    esp.softkey("Pad -")
    esp.wait_state(soundready=1, soundvalid=0, soundpad=15)
    esp.softkey("Pad +")
    esp.wait_state(
        soundready=1,
        soundvalid=1,
        soundpad=16,
        soundown=1,
        cutoff=1200,
    )
    esp.softkey("Back")
    esp.wait_state(page="Pad_Map", kitready=1, kitpad=16)
    esp.softkey("Save copy")
    esp.wait_state(kitview=1)
    esp.softkey("Confirm")
    esp.wait_state(kitready=1, kiterror=0, timeout=10)
    esp.softkey("Save copy")
    esp.wait_state(kitview=1)
    esp.softkey("Confirm")
    esp.wait_state(kitready=1, kiterror=9, timeout=10)
    path = "0:/wavex/instruments/" + name + ".wxi"
    daisy.msg(
        0x60,
        struct.pack(
            "<IBBH256sBBBhBB4x",
            345678,
            2,
            2,
            0,
            path.encode(),
            0,
            0,
            0,
            0,
            0,
            0,
        ),
    )
    deadline = time.monotonic() + 15
    while not daisy.tracks()[2].startswith("instrument"):
        assert time.monotonic() < deadline, daisy.tracks()
        time.sleep(0.2)
    esp.track(2)
    esp.wait_state(kitready=1, kiteditable=1, kitname=name.replace(" ", "_"))
    esp.page("PAD", 16)
    esp.wait_state(kitready=1, kitsample=a, kitchoke=1)
    esp.key("SHIFT")
    esp.wait_state(shift=1)
    esp.softkey("Sound")
    esp.wait_state(
        page="Pad_Sound",
        soundready=1,
        soundown=1,
        cutoff=1200,
        attack=7,
        decay=250,
        sustain=350,
    )
    esp.softkey("Inherit")
    esp.wait_state(
        soundready=1, soundown=0, cutoff=20000, attack=1, decay=50, sustain=800
    )
    esp.softkey("Back")
    esp.wait_state(page="Pad_Map", kitready=1)
    esp.page("PAD", 1)
    esp.wait_state(kitsample=0)
    # The selected pad audition reaches its sample end.
    daisy.wait_state(voices=1)
    esp.home()

    # A kit with only Pad 16 populated must be silent on note 60, then
    # sequence Pad 16 when the touch grid changes its note to 75.
    for track in range(16):
        daisy.msg(
            0x51,
            struct.pack("<BBBBHh", 4, track, 0, int(track == 2), 0, 0),
        )
    daisy.msg(0x51, struct.pack("<BBBBHh", 10, 2, 0, 0, 0, 0))
    esp.open_menu("Sequencer")
    esp.page("FOCUS", 3, 1)
    esp.wait_state(seqready=1, seqbits=0, seqnote=60)
    esp.page("LENGTH", 4)
    esp.wait_state(seqready=1, seqlen=4)
    esp.page("TOGGLE")
    esp.wait_state(seqready=1, seqbits=1)
    esp.softkey("Play")
    esp.wait_state(seqplaying=1)
    time.sleep(0.7)
    assert daisy.state()["voices"] == "1"
    esp.page("NOTE", 75)
    esp.wait_state(seqready=1, seqnote=75)
    daisy.wait_state(voices=lambda n: int(n) >= 2)
    esp.page("NOTE", 60)
    esp.wait_state(seqready=1, seqnote=60)
    daisy.wait_state(voices=1)
    esp.softkey("Stop")
    esp.wait_state(seqplaying=0)
    esp.home()
