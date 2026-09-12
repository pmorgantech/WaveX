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
def test_touch_envelope_and_matrix_drafts_apply_revert_and_recall(
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
    esp.wait_state(env=3, moddirty=1, attack=123, release=789)
    assert daisy.cmd("ENV", 0, 2)["revision"] == before["revision"]
    # A tab change cannot hide a pending draft.
    esp.page("TAB", "Mod")
    esp.wait_state(tab="Env", moddirty=1)
    esp.softkey("Revert")
    esp.wait_state(moddirty=0, attack=int(before["attack"]))
    esp.page("ATTACK", 123)
    esp.page("DECAY", 456)
    esp.page("SUSTAIN", 650)
    esp.page("RELEASE", 789)
    esp.softkey("Apply")
    esp.wait_state(modready=1, moddirty=0, moderror=0, attack=123, release=789)
    assert daisy.cmd("ENV", 0, 2)["attack"] == "123"
    esp.softkey("Mod")
    esp.wait_state(tab="Mod", modready=1)
    esp.page("SLOT", 8)
    esp.page("SOURCE", 16)
    esp.page("DEST", 3)
    esp.page("DEPTH", -16384)
    esp.page("CURVE", 1)
    esp.page("POLARITY", 1)
    esp.wait_state(slot=8, moddirty=1, source=16, moddepth=-16384, depth=2)
    esp.softkey("Apply")
    esp.wait_state(modready=1, moddirty=0, moderror=0, moddepth=-16384)
    route = daisy.cmd("MOD", 0, 7)
    assert route["source"] == "16"
    assert route["depth"] == "-16384"
    esp.softkey("Clear")
    esp.wait_state(moddirty=1, source=0, moddepth=0)
    esp.softkey("Revert")
    esp.wait_state(moddirty=0, source=16, moddepth=-16384, depth=2)
    name = "HIL env UI " + str(int(time.time()))
    _instrument(daisy, 851001, 5, name)
    _wait_osc(daisy, 0, 0, busy=0, completed=851001, error=0)
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
    esp.softkey("Mod")
    esp.wait_state(modready=1)
    esp.page("SLOT", 8)
    esp.wait_state(source=16, dest=3, moddepth=-16384, curve=1, polarity=1)
    esp.home()
