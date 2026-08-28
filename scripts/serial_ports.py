"""Locate USB serial devices by VID:PID without external dependencies.

Shared by serial_log.py and daisy_dfu_trigger.py. Everything here is stdlib so
the scripts run identically on the host and inside the devcontainer.
"""

import glob
import os

# Daisy Seed enumerates as an ST virtual COM port while the app runs, and as
# the STM32 DFU device once the bootloader takes over.
DAISY_CDC = ("0483", "5740")
DAISY_DFU = ("0483", "df11")


def _read(path):
    try:
        with open(path) as handle:
            return handle.read().strip()
    except OSError:
        return None


def usb_device_present(vid, pid):
    """True if a USB device with this VID:PID is currently enumerated."""
    for device in glob.glob("/sys/bus/usb/devices/*"):
        if _read(os.path.join(device, "idVendor")) == vid.lower() and _read(
            os.path.join(device, "idProduct")
        ) == pid.lower():
            return True
    return False


def find_tty(vid, pid):
    """Return the /dev node of the first tty belonging to this VID:PID.

    Walks /sys/class/tty rather than trusting a fixed /dev/ttyACM* number,
    which shifts depending on enumeration order.
    """
    for tty_path in sorted(glob.glob("/sys/class/tty/ttyACM*") + glob.glob("/sys/class/tty/ttyUSB*")):
        device = os.path.join(tty_path, "device")
        # Walk up to the USB device node that carries idVendor/idProduct.
        current = os.path.realpath(device)
        for _ in range(4):
            if _read(os.path.join(current, "idVendor")) == vid.lower() and _read(
                os.path.join(current, "idProduct")
            ) == pid.lower():
                return "/dev/" + os.path.basename(tty_path)
            parent = os.path.dirname(current)
            if parent == current:
                break
            current = parent
    return None
