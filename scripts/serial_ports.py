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

# Two USB ports reach the ESP32-P4 on the Waveshare ESP32-P4-WIFI6 board. The
# onboard CH343 USB-UART bridge carries the firmware console (UART0), so it is
# the port to log and monitor on. The P4's own USB connector enumerates as the
# chip's built-in USB-Serial/JTAG unit, which the ROM serves without firmware
# help: esptool can enter download mode, flash, and reset over it while the
# console stays undisturbed on the bridge. TinyUSB MIDI runs on the separate
# high-speed OTG controller, so it does not displace this unit while the app
# runs. Flash targets prefer "esp32-jtag" and fall back to "esp32".
ESP32_UART = ("1a86", "55d3")
ESP32_USB_JTAG = ("303a", "1001")

# The SRAM debug load reaches the Daisy through an ST-Link on SWD, not through
# the Daisy's own USB port, so the probe is what proves the debug path can
# work. Any ST-Link generation is fine: V2, V2-1, and the V3 variants.
STLINK = ("0483", ("3748", "374b", "374e", "374f", "3753", "3754"))

# Boards that expose a tty we can name on a command line. The Daisy in DFU has
# no tty - dfu-util claims it by VID:PID - and an ST-Link is a debug probe, so
# both are only valid for --present.
TTY_BOARDS = {
    "daisy": DAISY_CDC,
    "esp32": ESP32_UART,
    "esp32-jtag": ESP32_USB_JTAG,
}
USB_BOARDS = dict(TTY_BOARDS, **{"daisy-dfu": DAISY_DFU, "stlink": STLINK})


def _read(path):
    try:
        with open(path) as handle:
            return handle.read().strip()
    except OSError:
        return None


def _pids(pid):
    """Accept a single PID or a tuple of PIDs for one logical board."""
    return (pid,) if isinstance(pid, str) else tuple(pid)


def _describe(vid, pid):
    """Render VID:PID for messages, joining alternative PIDs with '/'."""
    return "%s:%s" % (vid, "/".join(_pids(pid)))


def usb_device_present(vid, pid):
    """True if a device with this VID and any of these PIDs is enumerated."""
    pids = tuple(p.lower() for p in _pids(pid))
    for device in glob.glob("/sys/bus/usb/devices/*"):
        if (
            _read(os.path.join(device, "idVendor")) == vid.lower()
            and _read(os.path.join(device, "idProduct")) in pids
        ):
            return True
    return False


def find_tty(vid, pid):
    """Return the /dev node of the first tty belonging to this VID:PID.

    Walks /sys/class/tty rather than trusting a fixed /dev/ttyACM* number,
    which shifts depending on enumeration order.
    """
    patterns = ("/sys/class/tty/ttyACM*", "/sys/class/tty/ttyUSB*")
    tty_paths = sorted(p for pattern in patterns for p in glob.glob(pattern))
    for tty_path in tty_paths:
        device = os.path.join(tty_path, "device")
        # Walk up to the USB device node that carries idVendor/idProduct.
        current = os.path.realpath(device)
        for _ in range(4):
            if (
                _read(os.path.join(current, "idVendor")) == vid.lower()
                and _read(os.path.join(current, "idProduct")) == pid.lower()
            ):
                return "/dev/" + os.path.basename(tty_path)
            parent = os.path.dirname(current)
            if parent == current:
                break
            current = parent
    return None


USAGE = (
    "usage: serial_ports.py <board>               print the tty for a board\n"
    "       serial_ports.py --present <board> [--wait SECONDS]\n"
    "                                             exit 0 if the board is"
    " enumerated\n"
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
    """Resolve boards for shell and make, so no caller hardcodes a ttyACM.

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
        del args[i]  # --wait
        del args[i]  # its value

    if args[:1] == ["--present"]:
        if len(args) != 2 or args[1] not in USB_BOARDS:
            sys.stderr.write(USAGE)
            return 2
        board = args[1]
        if _wait_present(board, timeout):
            return 0
        wanted = _describe(*USB_BOARDS[board])
        sys.stderr.write("no %s on USB (looked for %s)\n" % (board, wanted))
        return 1

    if len(args) != 1 or args[0] not in TTY_BOARDS:
        sys.stderr.write(USAGE)
        return 2
    board = args[0]
    port = find_tty(*TTY_BOARDS[board])
    if port is None:
        wanted = _describe(*TTY_BOARDS[board])
        sys.stderr.write("no %s tty (looked for USB %s)\n" % (board, wanted))
        return 1
    sys.stdout.write(port + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
