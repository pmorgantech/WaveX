"""HV-016: real Bank UI/SD transactions and selected-Track recall.

Replaces the live bench session, creates uniquely named Bank copies, and leaves
those files for inspection. Source WAVs and existing saved files are preserved.
BANKSTATS measures foreground service wall time, not callback cycles or MIDI
wire jitter; this small workload cannot close the full performance gate.
"""

import time

import pytest
from test_instrument_lfos import _lfo
from test_project_files import empty_project  # noqa: F401
from test_project_files import _track
from test_sequencer_tracks import (  # noqa: F401
    sequence_samples as sequence_samples_fixture,
)


def _banks(esp):
    esp.open_menu("Project")
    esp.wait_state(trackready=1)
    esp.key("SHIFT")
    esp.wait_state(shift=1)
    esp.softkey("Banks")
    esp.wait_state(page="Bank_Manager", bankready=1)
    if esp.state()["shift"] == "1":
        esp.key("SHIFT")
    esp.wait_state(shift=0)


def _files(esp, enabled):
    if esp.state()["shift"] != str(int(enabled)):
        esp.key("SHIFT")
    esp.wait_state(shift=int(enabled))


def _operation(esp, daisy, label, op, record_property, confirm=False):
    _files(esp, label in ("New", "Open", "Save copy", "Clear copy"))
    previous = daisy.cmd("BANKSTATS")["request"]
    start = time.monotonic()
    esp.softkey(label)
    if confirm:
        esp.wait_state(bankconfirm=op)
        esp.softkey("Confirm")
    deadline = start + 60
    while True:
        stats = daisy.cmd("BANKSTATS")
        if stats["request"] != previous and stats["busy"] == "0":
            break
        assert time.monotonic() < deadline, (label, stats, esp.state())
        time.sleep(0.1)
    assert stats["op"] == str(op) and stats["error"] == "0", stats
    assert int(stats["pumps"]) > 0 and int(stats["max_us"]) > 0, stats
    esp.wait_state(bankready=1, bankpending=0, bankconfirm=0, bankerror=0)
    # Distinct request IDs let repeated recall/copy measurements coexist.
    key = f"bank_{op}_{stats['request']}"
    record_property(key, repr(stats))
    elapsed_ms = round((time.monotonic() - start) * 1000)
    record_property(key + "_ui_wall_ms", elapsed_ms)
    return stats


@pytest.mark.both
@pytest.mark.sdcard
@pytest.mark.usefixtures("empty_project")
def test_bank_copy_recall_failure_isolation_and_service_timing(
    esp32, daisy, sequence_samples, record_property
):
    esp = esp32
    sample, second = sequence_samples
    daisy.bind_track(0, sample)
    daisy.bind_track(1, sample)
    state = daisy.cmd("LFO", 0, 0)
    values = (4, 8, 0, 1, 0.01, 0.125, 0.75)
    _lfo(daisy, 891001, int(state["revision"]), 0, values)
    saved_lfo = daisy.cmd("LFO", 0, 0)
    _track(esp, 1, trackloaded=1)
    for field, value, readback in (
        ("MIDI", 3, "midiin"),
        ("LEVEL", 3900, "mixgain"),
        ("PAN", 12000, "mixpan"),
    ):
        esp.page(field, value)
        esp.wait_state(trackready=1, mixready=1, **{readback: value})
    other_track = daisy.tracks()[1]
    daisy.note(1, 60)
    daisy.wait_state(voices=1)
    underruns = int(daisy.state()["underruns"])
    prefix = "HILB " + str(time.time_ns())[-15:]
    base, stored, copied, cleared = (prefix + suffix for suffix in "ABCD")

    _banks(esp)
    _files(esp, True)
    esp.page("NAME", base)
    _operation(esp, daisy, "New", 1, record_property)
    _files(esp, False)
    esp.wait_state(bankslot=1, bankoccupied=0)
    esp.softkey("Previous")
    esp.wait_state(bankready=1, bankslot=128, bankoccupied=0)
    esp.page("NAME", stored)
    _operation(esp, daisy, "Store copy", 4, record_property, confirm=True)
    esp.wait_state(bankoccupied=1, bankname=stored.replace(" ", "_"))
    assert daisy.state()["voices"] == "1"

    # Cancellation must leave the Track and retained job identity unchanged.
    prior = daisy.cmd("BANKSTATS")
    esp.softkey("Recall")
    esp.wait_state(bankconfirm=6)
    esp.softkey("Cancel")
    esp.wait_state(bankconfirm=0, bankready=1)
    assert daisy.cmd("BANKSTATS")["request"] == prior["request"]
    state = daisy.cmd("LFO", 0, 0)
    values = (0, 0, 1, 0, 100.0, 0.0, 0.0)
    _lfo(daisy, 891002, int(state["revision"]), 0, values)
    _operation(esp, daisy, "Recall", 6, record_property, confirm=True)
    restored = daisy.cmd("LFO", 0, 0)
    fields = ("wave", "sync", "retrigger", "follow", "rate", "delay", "fade")
    for key in fields:
        assert restored[key] == saved_lfo[key], (key, restored, saved_lfo)
    assert daisy.tracks()[1] == other_track
    assert daisy.state()["voices"] == "1"
    _track(esp, 1, trackloaded=1, midiin=3, mixgain=3900, mixpan=12000)

    _banks(esp)
    esp.softkey("Previous")
    esp.wait_state(bankready=1, bankslot=128, bankoccupied=1)
    _files(esp, True)
    esp.page("NAME", copied)
    _operation(esp, daisy, "Save copy", 3, record_property)
    esp.page("NAME", cleared)
    _operation(esp, daisy, "Clear copy", 5, record_property, confirm=True)
    esp.wait_state(bankoccupied=0)
    esp.page("NAME", stored)
    _operation(esp, daisy, "Open", 2, record_property)
    esp.wait_state(bankoccupied=1)
    # Existing destinations and missing sources must retain the active Bank.
    _files(esp, True)
    esp.softkey("Save copy")
    esp.wait_state(bankready=1, bankerror=4)
    esp.page("NAME", "Missing " + str(time.time_ns())[-14:])
    _files(esp, True)
    esp.softkey("Open")
    esp.wait_state(bankready=1, bankerror=3, bankname=stored.replace(" ", "_"))
    assert daisy.tracks()[1] == other_track
    # A fresh PCM admission must also leave an unrelated sounding Track intact.
    daisy.note(1, 60, on=False)
    daisy.bind_track(1, second)
    daisy.unload_sample(sample)
    daisy.note(1, 60)
    daisy.wait_state(voices=1)
    assert sample not in daisy.samples()
    _operation(esp, daisy, "Recall", 6, record_property, confirm=True)
    assert len(daisy.samples()) == 2
    assert daisy.state()["voices"] == "1"
    assert daisy.cmd("LFO", 0, 0)["rate"] == saved_lfo["rate"]
    assert int(daisy.state()["underruns"]) == underruns
    # Add a second occupied slot, then preload with one cold dependency and
    # one shared by the sounding Track. The selected empty slot is irrelevant.
    daisy.note(1, 60, on=False)
    daisy.bind_track(0, second)
    _files(esp, False)
    esp.softkey("Next")
    esp.wait_state(bankready=1, bankslot=1, bankoccupied=0)
    esp.page("NAME", prefix + "E")
    _operation(esp, daisy, "Store copy", 4, record_property, confirm=True)
    for sid in daisy.samples():
        if sid != second:
            daisy.unload_sample(sid)
    before_tracks = daisy.tracks()
    before_edit = daisy.cmd("EDIT", 0)
    resident = set(daisy.samples())
    assert resident == {second}
    daisy.note(1, 60)
    daisy.wait_state(voices=1)
    _files(esp, False)
    esp.softkey("Next")
    esp.wait_state(bankready=1, bankslot=2, bankoccupied=0)
    _operation(esp, daisy, "Preload", 7, record_property)
    assert daisy.tracks() == before_tracks
    assert daisy.cmd("EDIT", 0)["revision"] == before_edit["revision"]
    assert daisy.state()["voices"] == "1"
    preloaded = set(daisy.samples())
    assert len(preloaded) == 2 and resident < preloaded
    _operation(esp, daisy, "Preload", 7, record_property)
    # Shared/repeated paths deduplicate.
    assert set(daisy.samples()) == preloaded
    assert int(daisy.state()["underruns"]) == underruns
    daisy.note(1, 60, on=False)
    esp.home()
