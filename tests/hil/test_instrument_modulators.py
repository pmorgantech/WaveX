"""Instrument-owned envelope/matrix readback and WXI recall."""

import struct
import time

import pytest
from test_oscillators import _instrument, _wait_osc
from test_sequencer_tracks import (  # noqa: F401
    sequence_samples as sequence_samples_fixture,
)


def _edit(daisy, request, revision, op, index, envelope, slot):
    daisy.msg(
        0x6C,
        struct.pack(
            "<IIBBBBffffBBhBB",
            request,
            revision,
            0,
            index,
            op,
            0,
            *envelope,
            *slot,
        ),
    )


@pytest.mark.daisy
@pytest.mark.sdcard
def test_instrument_envelope_three_and_matrix_survive_wxi_recall(
    daisy, sequence_samples
):
    daisy.bind_track(0, sequence_samples[0])
    before = daisy.cmd("ENV", 0, 2)
    envelope = (0.125, 0.25, 0.5, 0.25)
    slot = (16, 3, -25000, 0, 0)
    _edit(daisy, 850001, int(before["revision"]), 1, 2, envelope, slot)
    applied = daisy.cmd("ENV", 0, 2)
    assert applied["completed"] == "850001"
    assert applied["error"] == "0"
    assert applied["attack"] == "125"
    assert applied["sustain"] == "500"
    _edit(daisy, 850001, int(before["revision"]), 1, 2, envelope, slot)
    assert daisy.cmd("ENV", 0, 2)["revision"] == applied["revision"]
    _edit(daisy, 850002, int(before["revision"]), 2, 7, envelope, slot)
    assert daisy.cmd("MOD", 0, 7)["error"] == "1"
    _edit(daisy, 850003, int(applied["revision"]), 2, 7, envelope, slot)
    route = daisy.cmd("MOD", 0, 7)
    assert route["source"] == "16"
    assert route["depth"] == "-25000"
    assert route["error"] == "0"
    name = "HIL envs " + str(int(time.time()))
    _instrument(daisy, 850004, 5, name)
    _wait_osc(daisy, 0, 0, busy=0, completed=850004, error=0)
    daisy.bind_track(0, 0)
    _instrument(daisy, 850005, 2, "0:/wavex/instruments/" + name + ".wxi")
    _wait_osc(daisy, 0, 0, busy=0, valid=1, zones=1)
    assert daisy.cmd("ENV", 0, 2)["attack"] == "125"
    assert daisy.cmd("ENV", 0, 2)["release"] == "250"
    assert daisy.cmd("MOD", 0, 7)["depth"] == "-25000"


@pytest.mark.both
@pytest.mark.sdcard
def test_touch_sound_previews_apply_revert_and_recall(  # noqa: E501
    esp32, daisy, sequence_samples
):
    esp = esp32
    # Binding a bare sample retains the Track's editable Instrument settings.
    # Create an Instrument so this test is repeatable without rebooting.
    _instrument(daisy, 852001, 4, "HIL mod UI fresh")
    _wait_osc(daisy, 0, 0, completed=852001, error=0)
    _instrument(daisy, 852002, 7, sample=sequence_samples[0])
    _wait_osc(daisy, 0, 0, completed=852002, error=0, zones=1)
    esp.home()
    esp.track(0)
    esp.open_menu("Instrument")
    esp.wait_state(oscready=1, oscvalid=1)
    esp.page("TAB", "Env")
    esp.wait_state(tab="Env", modready=1, modvalid=1, env=1)
    esp.page("ENV", 3)
    before = daisy.cmd("ENV", 0, 2)
    esp.page("ATTACK", 123)
    esp.page("DECAY", 456)
    esp.page("SUSTAIN", 650)
    esp.page("RELEASE", 789)
    esp.wait_state(
        env=3, moddirty=0, editdirty=1, editpending=0, attack=123, release=789
    )
    assert daisy.cmd("ENV", 0, 2)["attack"] == "123"
    # The audible edit and undo point survive tab navigation.
    esp.page("TAB", "Mod")
    esp.wait_state(tab="Mod", editdirty=1, editpending=0)
    esp.page("TAB", "Env")
    esp.wait_state(tab="Env", env=3)
    esp.softkey("Revert")
    esp.wait_state(  # noqa: E501
        moddirty=0, editdirty=0, editpending=0, attack=int(before["attack"])
    )
    assert daisy.cmd("ENV", 0, 2)["attack"] == before["attack"]
    esp.page("ATTACK", 123)
    esp.page("DECAY", 456)
    esp.page("SUSTAIN", 650)
    esp.page("RELEASE", 789)
    esp.wait_state(editdirty=1, editpending=0)
    esp.softkey("Apply")
    esp.wait_state(
        modready=1,
        moddirty=0,
        moderror=0,
        editdirty=0,
        editpending=0,
        attack=123,
        release=789,
    )
    assert daisy.cmd("ENV", 0, 2)["attack"] == "123"
    esp.softkey("Mod")
    esp.wait_state(tab="Mod", modready=1)
    esp.page("SLOT", 8)
    esp.page("SOURCE", 16)
    esp.page("DEST", 3)
    esp.page("DEPTH", -16384)
    esp.page("CURVE", 1)
    esp.page("POLARITY", 1)
    esp.wait_state(
        slot=8,
        moddirty=0,
        editdirty=1,
        editpending=0,
        source=16,
        moddepth=-16384,
        depth=2,
    )
    assert daisy.cmd("MOD", 0, 7)["depth"] == "-16384"
    esp.softkey("Apply")
    esp.wait_state(
        modready=1,
        moddirty=0,
        moderror=0,
        editdirty=0,
        editpending=0,
        moddepth=-16384,  # noqa: E501
    )
    route = daisy.cmd("MOD", 0, 7)
    assert route["source"] == "16"
    assert route["depth"] == "-16384"
    esp.softkey("Clear")
    esp.wait_state(  # noqa: E501
        moddirty=0, editdirty=1, editpending=0, source=0, moddepth=0
    )
    esp.softkey("Revert")
    esp.wait_state(
        moddirty=0,
        editdirty=0,
        editpending=0,
        source=16,
        moddepth=-16384,
        depth=2,  # noqa: E501
    )
    esp.page("TAB", "Amp")
    esp.wait_state(tab="Amp", editready=1, editpending=0)
    esp.page("LEVEL", 350)
    esp.page("PAN", 250)
    esp.wait_state(editdirty=1, editpending=0, instgain=350, instpan=250)
    assert daisy.cmd("EDIT", 0)["gain"] == "350"
    esp.page("TAB", "Filter")
    esp.wait_state(tab="Filter", editready=1, editpending=0)
    baseline_cutoff = daisy.cmd("EDIT", 0)["cutoff"]
    esp.page("CUTOFF", 24000)
    esp.wait_state(editdirty=1, editpending=0, instcutoff=24000)
    assert daisy.cmd("EDIT", 0)["cutoff"] != baseline_cutoff
    esp.key("SHIFT")
    esp.softkey("Revert")
    esp.wait_state(editdirty=0, editpending=0, instgain=1000, instpan=500)
    assert daisy.cmd("EDIT", 0)["cutoff"] == baseline_cutoff
    esp.page("TAB", "Amp")
    esp.wait_state(tab="Amp", editready=1, editpending=0)
    esp.page("LEVEL", 650)
    esp.wait_state(editdirty=1, editpending=0, instgain=650)
    name = "HIL env UI " + str(int(time.time()))
    _instrument(daisy, 851001, 5, name)
    _wait_osc(daisy, 0, 0, busy=0, completed=851001, error=0)
    assert daisy.cmd("EDIT", 0)["dirty"] == "0"
    esp.home()
    daisy.bind_track(0, 0)
    _instrument(daisy, 851002, 2, "0:/wavex/instruments/" + name + ".wxi")
    _wait_osc(daisy, 0, 0, busy=0, valid=1, zones=1)
    esp.open_menu("Instrument")
    esp.wait_state(oscready=1, oscvalid=1)
    esp.page("TAB", "Env")
    esp.wait_state(modready=1)
    esp.page("ENV", 3)
    esp.wait_state(attack=123, decay=456, sustain=650, release=789)
    assert daisy.cmd("EDIT", 0)["gain"] == "650"
    esp.softkey("Mod")
    esp.wait_state(modready=1)
    esp.page("SLOT", 8)
    esp.wait_state(source=16, dest=3, moddepth=-16384, curve=1, polarity=1)
    esp.home()


@pytest.mark.both
@pytest.mark.sdcard
def test_midi_expression_routes_by_track_and_resets(  # noqa: E501
    esp32, daisy, sequence_samples
):
    from test_sequencer_solo import _peak

    esp = esp32
    _instrument(daisy, 853001, 9, "HIL MIDI expression")
    _wait_osc(daisy, 0, 0, completed=853001, error=0)
    daisy.bind_track(0, sequence_samples[0])
    _wait_osc(daisy, 0, 0, busy=0, valid=1, zones=1)
    daisy.set_midi_in(0, 2)
    daisy.msg(0x78, struct.pack("<BBH", 5, 0, 0))
    daisy.msg(0x78, struct.pack("<BBH", 8, 0, 0))
    daisy.msg(0x78, struct.pack("<BBH", 1, 0, 16384))
    daisy.msg(0x78, struct.pack("<BBH", 2, 0, 32768))
    esp.home()
    esp.track(0)
    esp.open_menu("Instrument")
    esp.wait_state(oscready=1, oscvalid=1)
    esp.page("TAB", "Env")
    esp.wait_state(modready=1, modvalid=1)
    esp.page("SUSTAIN", 1000)
    esp.wait_state(moddirty=0, editpending=0)
    esp.softkey("Apply")
    esp.wait_state(editdirty=0, editpending=0)
    esp.page("TAB", "Mod")
    esp.wait_state(modready=1, modvalid=1)
    esp.page("SOURCE", 12)
    esp.page("DEST", 4)  # Pan makes forwarding observable at codec meters.
    esp.page("DEPTH", 32767)
    esp.wait_state(moddirty=0, editpending=0, source=12, dest=4)
    esp.softkey("Apply")
    esp.wait_state(editdirty=0, editpending=0)
    daisy.note(0, 60)
    daisy.wait_state(voices=1)

    def centered():
        left, right = _peak(daisy)
        assert min(left, right) > 100, (left, right)
        assert abs(left - right) < max(left, right) * 0.05, (left, right)

    def right_only():
        left, right = _peak(daisy)
        assert right > 100 and left < right * 0.01, (left, right)

    try:
        centered()
        esp.cmd("MIDICC", 1, 1, 127)  # Non-matching channel has no effect.
        centered()
        esp.cmd("MIDICC", 2, 1, 127)
        right_only()
        esp.cmd("MIDICC", 2, 121, 0)
        centered()
        esp.page("SOURCE", 13)
        esp.wait_state(moddirty=0, editpending=0, source=13)
        esp.softkey("Apply")
        esp.wait_state(editdirty=0, editpending=0)
        esp.cmd("MIDIPRESSURE", 1, 127)
        centered()
        esp.cmd("MIDIPRESSURE", 2, 127)
        right_only()
        daisy.set_midi_in(0, 0)  # Routing change clears the old value.
        centered()
        esp.cmd("MIDIPRESSURE", 16, 127)  # Omni accepts any channel.
        right_only()
        daisy.set_midi_in(0, 255)
        esp.cmd("MIDIPRESSURE", 16, 127)  # Off ignores incoming expression.
        centered()
        assert daisy.state()["underruns"] == "0"
    finally:
        daisy.note(0, 60, on=False)
        daisy.set_midi_in(0, 1)
        esp.home()
