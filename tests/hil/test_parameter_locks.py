"""Touch lock editing, readback and pattern recall on both boards."""

import time

import pytest
from test_pattern_files import _files, _new


@pytest.mark.both
@pytest.mark.sdcard
def test_parameter_locks_edit_clear_and_survive_pattern_recall(esp32, daisy):
    esp = esp32
    esp.open_menu("Sequencer")
    esp.wait_state(seqready=1)
    _files(esp)
    _new(esp)
    esp.softkey("Back")
    esp.wait_state(seqready=1)
    esp.page("FOCUS", 1, 1)
    esp.wait_state(seqready=1)
    esp.page("TOGGLE")
    esp.wait_state(seqready=1, seqbits=1)
    esp.key("SHIFT")
    esp.wait_state(shift=1)
    esp.softkey("Locks")
    esp.wait_state(seqlocks=1, lockslot=1, lockparam=0, sk2en=0, sk3en=1)
    for slot in range(2, 5):
        esp.softkey("Lock +")
        esp.wait_state(lockslot=slot, sk2en=1, sk3en=int(slot < 4))
    for slot in range(3, 0, -1):
        esp.softkey("Lock -")
        esp.wait_state(lockslot=slot, sk2en=int(slot > 1), sk3en=1)
    locks = [(2, 12345), (3, 45000), (8, 65535), (9, 32768)]
    for slot, (parameter, value) in enumerate(locks, 1):
        esp.page("SLOT", slot)
        esp.page("LOCK", parameter, value)
        esp.wait_state(
            seqready=1,
            lockslot=slot,
            lockparam=parameter,
            lockvalue=value,
        )
    esp.page("FOCUS", 1, 2)
    esp.wait_state(seqready=1, lockparam=0, lockvalue=0)
    esp.page("FOCUS", 1, 1)
    esp.wait_state(seqready=1, lockparam=9, lockvalue=32768)
    esp.softkey("Clear lock")
    esp.wait_state(seqready=1, lockparam=0, lockvalue=0)
    esp.page("SLOT", 1)
    esp.wait_state(lockparam=2, lockvalue=12345)
    esp.softkey("Grid")
    esp.wait_state(seqlocks=0, seqbits=1)
    _files(esp)
    name = "HIL locks " + str(int(time.time()))
    esp.page("NAME", name)
    esp.softkey("Save copy")
    esp.wait_state(fileready=1, fileerror=0, timeout=10)
    _new(esp)
    esp.page("NAME", name)
    esp.softkey("Load")
    esp.wait_state(fileconfirm=2)
    esp.softkey("Confirm")
    esp.wait_state(fileready=1, fileerror=0, timeout=10)
    esp.softkey("Back")
    esp.wait_state(seqready=1, seqbits=1)
    esp.key("SHIFT")
    esp.wait_state(shift=1)
    esp.softkey("Locks")
    esp.wait_state(seqlocks=1, lockparam=2, lockvalue=12345)
    for slot, (parameter, value) in enumerate(locks[:3], 1):
        esp.page("SLOT", slot)
        esp.wait_state(seqready=1, lockparam=parameter, lockvalue=value)
    esp.page("SLOT", 4)
    esp.wait_state(lockparam=0, lockvalue=0)
    esp.softkey("Grid")
    esp.wait_state(seqlocks=0, shift=0)
    _files(esp)
    _new(esp)
    esp.softkey("Back")
    esp.wait_state(seqready=1, seqbits=0)
    esp.home()
    daisy.wait_state(underruns=0, dropped=0)
