"""Both recording source families share one acknowledged take workflow."""

import time

import pytest


@pytest.mark.both
@pytest.mark.sdcard
@pytest.mark.parametrize("source", [0, 3])
def test_capture_audition_save_and_assign(at_home, daisy, source):
    esp = at_home
    esp.track(15)
    esp.open_menu("Sample")
    esp.page("TAB", "Record")
    initial = esp.wait_state(tab="Record", recready="1")
    if initial["recstate"] == "4":
        esp.softkey("Done" if initial["recsaved"] == "1" else "Discard")
    esp.wait_state(recready="1", recstate="0")
    esp.page("SOURCE", source)
    esp.page("NAME", f"HIL take {time.time_ns()} {source}")
    esp.softkey("Arm")
    esp.wait_state(recready="1", recstate="1")
    esp.softkey("Start")
    esp.wait_state(
        recready="1",
        recstate="2",
        recframes=lambda n: int(n) >= 4800,
    )
    esp.softkey("Stop")
    take = esp.wait_state(
        recready="1",
        recstate="4",
        recsample=lambda n: int(n) > 0,
    )
    assert take["recerror"] == "0", take
    sample = int(take["recsample"])
    esp.softkey("Audition")
    time.sleep(0.15)
    esp.wait_state(recready="1", recstate="4", recerror="0", sk1en="1")
    esp.softkey("Stop")
    time.sleep(0.15)
    esp.wait_state(recready="1", recstate="4", sk4en="1")
    esp.softkey("Save")
    time.sleep(0.2)
    saved = esp.wait_state(timeout=45, recready="1", recstate="4")
    assert saved["recsaved"] == "1", saved
    assert saved["recerror"] == "0", saved
    assert int(saved["recsample"]) == sample
    esp.key("SHIFT")
    esp.softkey("To keys" if source == 3 else "To pad")
    prefix = "key" if source == 3 else "kit"
    esp.wait_state(**{prefix + "ready": "1", "assigntake": sample})
    esp.softkey("New keys" if source == 3 else "New kit")
    esp.softkey("Confirm")
    esp.wait_state(**{prefix + "view": "1"})
    esp.page("NAME", "HIL recorded assignment")
    esp.softkey("Confirm")
    esp.wait_state(**{prefix + "ready": "1", prefix + "editable": "1"})
    esp.softkey("Assign take")
    esp.wait_state(**{prefix + "ready": "1", prefix + "sample": sample})
    note = 60
    daisy.note(15, note, 100, True)
    daisy.note(15, note, 0, False)
    esp.home()
    daisy.unbind_track(15)
    daisy.unload_sample(sample)
