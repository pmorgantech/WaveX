#!/usr/bin/env python3
"""Reboot the running Daisy firmware into DFU mode - no BOOT/RESET presses.

The firmware watches its USB CDC port for the token below and answers with
System::ResetToBootloader(DAISY_INFINITE_TIMEOUT), so the Daisy bootloader
comes up in DFU and waits indefinitely for dfu-util. See the DFU block in
firmware/daisy/src/main.cpp.

Exits non-zero if the Daisy never appears in DFU, in which case the firmware
is probably wedged and the manual BOOT+RESET path (`make daisy-flash`) is the
recovery route.
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from serial_ports import DAISY_CDC, DAISY_DFU, find_tty, usb_device_present  # noqa: E402

TOKEN = b"WAVEX-ENTER-DFU"
DFU_WAIT_SECONDS = 10.0


def main():
    if usb_device_present(*DAISY_DFU):
        print("Daisy is already in DFU mode - nothing to trigger.")
        return 0

    port = find_tty(*DAISY_CDC)
    if port is None:
        print(
            "Could not find the Daisy CDC port (%s:%s).\n"
            "Is it powered and running WaveX firmware? If the firmware is\n"
            "wedged, flash manually: hold BOOT, tap RESET, then `make daisy-flash`."
            % DAISY_CDC,
            file=sys.stderr,
        )
        return 1

    print("Sending DFU trigger to %s..." % port)
    try:
        fd = os.open(port, os.O_WRONLY | os.O_NOCTTY)
        try:
            os.write(fd, TOKEN)
        finally:
            os.close(fd)
    except OSError as exc:
        print(
            "Failed to write to %s: %s\n"
            "If a terminal (minicom/tail) holds the port exclusively, close it first."
            % (port, exc),
            file=sys.stderr,
        )
        return 1

    deadline = time.time() + DFU_WAIT_SECONDS
    while time.time() < deadline:
        if usb_device_present(*DAISY_DFU):
            # The bootloader has just enumerated; give udev a moment to apply
            # permissions before dfu-util claims the device.
            time.sleep(0.5)
            print("Daisy is in DFU mode.")
            return 0
        time.sleep(0.2)

    print(
        "Daisy did not enter DFU within %.0fs.\n"
        "Fall back to the manual sequence: hold BOOT, tap RESET, then `make daisy-flash`."
        % DFU_WAIT_SECONDS,
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
