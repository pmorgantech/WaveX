"""Debug SD probes: exact readback and exclusion of live driver-mode changes.

Run after a fresh boot on mounted test media, with other storage work idle.
Scratch files are exclusively created and removed only after verified readback.
"""

import time

import pytest
from wavex_target import TargetError


@pytest.mark.daisy
@pytest.mark.sdcard
@pytest.mark.parametrize("pattern", [0, 1, 2, 3])
def test_pattern_readback_and_driver_mode_busy_guard(daisy, pattern):
    first_error = daisy.cmd("SDIO")
    assert first_error["valid"] == "0", "Fresh boot required after SD faults"
    initial = daisy.cmd("SDIO", "INFO")
    daisy.cmd("SDTEST", "START", 65536, 512, 4, 44, pattern, 0, 5)
    with pytest.raises(TargetError, match="busy_or_invalid"):
        daisy.cmd("SDIO", "MODE", 3)
    deadline = time.monotonic() + 15
    while True:
        status = daisy.cmd("SDTEST", "STATUS")
        if status["busy"] == "0":
            break
        assert time.monotonic() < deadline, status
        time.sleep(0.05)
    assert status["phase"] == "9", status
    assert status["offset"] == "65536", status
    assert status["pattern"] == str(pattern), status
    assert daisy.cmd("SDIO")["valid"] == "0"
    assert daisy.cmd("SDIO", "INFO")["mode"] == initial["mode"]
