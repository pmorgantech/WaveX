"""Locate USB serial devices by VID:PID without external dependencies.

Shared by serial_log.py and daisy_dfu_trigger.py. Everything here is stdlib so
the scripts run identically on the host and inside the devcontainer.
"""

import glob
import os
import sys
import time

# Daisy Seed enumerates as an ST virtual COM port while the app runs, and as
# the STM32 DFU device once the bootloader takes over.
DAISY_CDC = ("0483", "5740")
DAISY_DFU = ("0483", "df11")

# The Waveshare ESP32-P4-WIFI6 board flashes over an onboard CH343 USB-UART
# bridge. The P4 itself has no USB-serial-JTAG path we use, so this bridge is
# the only port esptool can talk to.
ESP32_UART = ("1a86", "55d3")

# Boards that expose a tty we can name on a command line. The Daisy in DFU has
# no tty - dfu-util claims it by VID:PID - so it is only valid for --present.
TTY_BOARDS = {
    "daisy": DAISY_CDC,
    "esp32": ESP32_UART,
}
USB_BOARDS = dict(TTY_BOARDS, **{"daisy-dfu": DAISY_DFU})


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


USAGE = (
    "usage: serial_ports.py <board>               print the tty for a board\n"
    "       serial_ports.py --present <board> [--wait SECONDS]\n"
    "                                             exit 0 if the board is enumerated\n"
    "boards: tty %s | usb %s\n"
) % ("|".join(sorted(TTY_BOARDS)), "|".join(sorted(USB_BOARDS)))


def _wait_present(board, timeout):
    """Poll for a board to enumerate. Used to give a human time to hit BOOT."""
    deadline = time.time() + timeout
    while True:
        if usb_device_present(*USB_BOARDS[board]):
            return True
        if time.time() >= deadline:
            return False
        time.sleep(0.2)


def main(argv):
    """Resolve boards for shell and make, so no caller hardcodes a ttyACM number.

    Exits non-zero with a message on stderr when the board is absent, which is
    what makes a missing board fail loudly instead of acting on the wrong one.
    """
    args = argv[1:]
    timeout = 0.0
    if "--wait" in args:
        i = args.index("--wait")
        try:
            timeout = float(args[i + 1])
        except (IndexError, ValueError):
            sys.stderr.write(USAGE)
            return 2
        del args[i : i + 2]

    if args[:1] == ["--present"]:
        if len(args) != 2 or args[1] not in USB_BOARDS:
            sys.stderr.write(USAGE)
            return 2
        board = args[1]
        if _wait_present(board, timeout):
            return 0
        sys.stderr.write(
            "no %s on USB (looked for %s:%s)\n" % ((board,) + USB_BOARDS[board])
        )
        return 1

    if len(args) != 1 or args[0] not in TTY_BOARDS:
        sys.stderr.write(USAGE)
        return 2
    board = args[0]
    port = find_tty(*TTY_BOARDS[board])
    if port is None:
        sys.stderr.write(
            "no %s tty found (looked for USB %s:%s)\n" % ((board,) + TTY_BOARDS[board])
        )
        return 1
    sys.stdout.write(port + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
