"""Keyboard zone editing, staged ranges and WXI recall on both boards."""

import time

import pytest
from test_sequencer_tracks import _pattern
from test_sequencer_tracks import (  # noqa: F401
    sequence_samples as sequence_samples_fixture,
)


def open_keys(esp, track, oscillator=1):
    esp.home()
    esp.track(track)
    esp.open_menu("Instrument")
    esp.wait_state(oscready=1)
    if oscillator != 1:
        esp.page("OSC", oscillator)
        esp.wait_state(oscready=1, osc=oscillator)
    esp.key("SHIFT")
    esp.wait_state(shift=1)
    esp.softkey("Key Map")
    return esp.wait_state(page="Key_Map", keyready=1)


def stage(esp, **values):
    for field, value in values.items():
        esp.page(field, value)
    esp.wait_state(keydirty=1)
    esp.softkey("Apply")
    return esp.wait_state(keyready=1, keydirty=0, keyerror=0)


@pytest.mark.both
@pytest.mark.sdcard
def test_key_map_velocity_split_staging_and_saved_recall(
    esp32, daisy, sequence_samples
):
    esp = esp32
    a, b = sequence_samples
    daisy.bind_track(1, a)
    daisy.note(1, 60)
    daisy.wait_state(voices=1)
    open_keys(esp, 0)
    esp.softkey("New keys")
    esp.wait_state(keyview=3)
    esp.softkey("Confirm")
    esp.wait_state(keyview=1)
    name = "HIL keys " + str(int(time.time()))
    esp.page("NAME", name)
    esp.softkey("Confirm")
    esp.wait_state(keyready=1, keyeditable=1, keyname=name.replace(" ", "_"))
    assert daisy.state()["voices"] == "1"
    # Build a sparse split using first and last slots.
    for zone, choice, lo, hi in [(1, 1, 1, 63), (32, 2, 64, 127)]:
        esp.page("ZONE", zone)
        esp.wait_state(keyzone=zone)
        esp.softkey("Assign")
        esp.wait_state(keyview=2, keypick0=a)
        esp.page("CHOOSE", choice)
        esp.wait_state(keyready=1, keysample=a if choice == 1 else b)
        stage(esp, KEYLO=48, KEYHI=72, VELLO=lo, VELHI=hi, ROOT=60)
    # Touch edits do not reach the engine until Apply.
    esp.page("KEYLO", 61)
    esp.wait_state(keydirty=1, keylo=61)
    daisy.note(0, 60, 100)
    daisy.wait_state(voices=2)
    daisy.note(0, 60, on=False)
    daisy.wait_state(voices=1)
    esp.softkey("Revert")
    esp.wait_state(keydirty=0, keylo=48)
    # Both inclusive sides resolve exactly one voice, never both layers.
    for velocity in (1, 63, 64, 127):
        daisy.note(0, 60, velocity)
        daisy.wait_state(voices=2)
        daisy.note(0, 60, on=False)
        daisy.wait_state(voices=1)
    for note in (47, 73):
        daisy.note(0, note, 100)
        time.sleep(0.12)
        assert daisy.state()["voices"] == "1"
    # New range affects future triggers; an existing held voice survives.
    daisy.note(0, 60, 100)
    daisy.wait_state(voices=2)
    stage(esp, KEYLO=61)
    assert daisy.state()["voices"] == "2"
    daisy.note(0, 60, on=False)
    daisy.wait_state(voices=1)
    daisy.note(0, 60, 100)
    time.sleep(0.12)
    assert daisy.state()["voices"] == "1"
    stage(esp, KEYLO=48)
    esp.softkey("Save copy")
    esp.wait_state(keyview=1)
    esp.softkey("Confirm")
    esp.wait_state(keyready=1, keyerror=0, timeout=10)
    esp.home()
    esp.track(2)
    esp.open_menu("Project")
    esp.softkey("Assign")
    esp.wait_state(page="Instrument_Browser")
    esp.softkey("Saved")
    esp.wait_state(dir="/wavex/instruments", entries=lambda n: int(n) > 1)
    esp.page("SEL", name + ".wxi")
    esp.wait_state(sel=(name + ".wxi").replace(" ", "_"), sk2="Load")
    esp.softkey("Load")
    esp.wait_state(picker=1)
    esp.softkey("Load")
    esp.wait_state(status="Instrument_loaded", timeout=15)
    open_keys(esp, 2)
    esp.wait_state(
        keyname=name.replace(" ", "_"),
        keysample=a,
        keylo=48,
        keyhi=72,
        vello=1,
        velhi=63,
    )
    esp.page("ZONE", 32)
    esp.wait_state(
        keyzone=32,
        keyfirst=25,
        keysample=b,
        keylo=48,
        keyhi=72,
        vello=64,
        velhi=127,
        rootnote=60,
    )
    esp.key("SHIFT")
    esp.wait_state(shift=1)
    esp.softkey("Clear zone")
    esp.wait_state(keyready=1, keysample=0)
    assert a in daisy.samples() and b in daisy.samples()
    # Clearing the copy leaves the original Track's high-velocity layer.
    daisy.note(0, 60, 100)
    daisy.wait_state(voices=2)
    daisy.note(0, 60, on=False)
    daisy.wait_state(voices=1)
    # The touch grid resolves the saved low-velocity zone on the backend.
    _pattern(daisy, (2,), length=4)
    esp.open_menu("Sequencer")
    esp.page("FOCUS", 3, 1)
    esp.wait_state(seqready=1)
    esp.page("NOTE", 60)
    esp.wait_state(seqready=1, seqnote=60)
    esp.page("VELOCITY", 64)
    esp.wait_state(seqready=1, seqvel=64)
    esp.softkey("Play")
    esp.wait_state(seqplaying=1)
    time.sleep(0.7)
    assert daisy.state()["voices"] == "1"
    esp.page("VELOCITY", 63)
    esp.wait_state(seqready=1, seqvel=63)
    daisy.wait_state(voices=2)
    esp.softkey("Stop")
    esp.wait_state(seqplaying=0)
    daisy.note(2, 60, on=False)
    daisy.wait_state(voices=1)
    esp.home()


@pytest.mark.both
@pytest.mark.sdcard
def test_second_oscillator_key_map_assignment_ranges_and_wxi_recall(
    esp32, daisy, sequence_samples
):
    from test_oscillators import _instrument, _wait_osc

    esp = esp32
    a, b = sequence_samples
    open_keys(esp, 0)
    esp.softkey("New keys")
    esp.wait_state(keyview=3)
    esp.softkey("Confirm")
    esp.wait_state(keyview=1)
    name = "HIL 2keys " + str(int(time.time()))
    esp.page("NAME", name)
    esp.softkey("Confirm")
    esp.wait_state(keyready=1, keyeditable=1)
    for oscillator, choice, lo, hi in [(1, 1, 48, 60), (2, 2, 61, 72)]:
        if oscillator == 2:
            open_keys(esp, 0, oscillator)
        esp.wait_state(keyready=1, keyosc=oscillator, keysample=0)
        esp.softkey("Assign")
        esp.wait_state(keyview=2, keypick0=a)
        esp.page("CHOOSE", choice)
        esp.wait_state(keyready=1, keysample=a if oscillator == 1 else b)
        stage(esp, KEYLO=lo, KEYHI=hi, VELLO=1, VELHI=127, ROOT=60)
    esp.softkey("Save copy")
    esp.wait_state(keyview=1)
    esp.softkey("Confirm")
    esp.wait_state(keyready=1, keyerror=0, timeout=10)
    esp.home()
    daisy.bind_track(0, 0)
    _instrument(daisy, 840001, 2, "0:/wavex/instruments/" + name + ".wxi")
    _wait_osc(daisy, 0, 1, valid=1, busy=0, zones=1, type=1)
    open_keys(esp, 0)
    esp.wait_state(keyready=1, keyosc=1, keysample=a, keylo=48, keyhi=60)
    open_keys(esp, 0, 2)
    esp.wait_state(keyready=1, keyosc=2, keysample=b, keylo=61, keyhi=72)
    # Mix remains independent of zone assignment/save.
    esp.home()
    esp.open_menu("Instrument")
    esp.wait_state(oscready=1)
    esp.page("MIX", 500)
    esp.key("SHIFT")
    esp.wait_state(shift=1)
    esp.softkey("Apply")
    esp.wait_state(oscready=1, oscdirty=0, oscmix=500)
    for note in (48, 60, 61, 72):
        daisy.note(0, note, 100)
        daisy.wait_state(voices=1)
        daisy.note(0, note, on=False)
        daisy.wait_state(voices=0)
    for note in (47, 73):
        daisy.note(0, note, 100)
        time.sleep(0.12)
        assert daisy.state()["voices"] == "0"
    esp.home()
