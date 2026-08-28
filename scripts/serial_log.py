#!/usr/bin/env python3
"""Stream a serial port into a logfile you can `tail -f` - a minicom replacement.

minicom holds the port for a human, which is what makes flashing and automated
testing fight over it. This writes to a file instead, and reconnects on its own
when the device disappears (the Daisy re-enumerates on every reset and every
DFU cycle), so the log survives a flash without anyone restarting anything.

Usage:
  serial_log.py --port /dev/ttyACM0 --baud 115200 --out logs/esp32.log
  serial_log.py --vid 0483 --pid 5740 --out logs/daisy.log
"""

import argparse
import errno
import os
import signal
import sys
import termios
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from serial_ports import find_tty  # noqa: E402

_running = True


def _stop(_signum, _frame):
    global _running
    _running = False


def open_port(port, baud):
    """Open a tty in raw mode at `baud`, without hanging up on close.

    Clearing HUPCL matters: with it set, closing the port drops DTR, which on
    an ESP32 auto-reset circuit reboots the chip every time this script exits.
    """
    fd = os.open(port, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        attrs = termios.tcgetattr(fd)
        iflag, oflag, cflag, lflag, ispeed, ospeed, cc = attrs
        iflag = 0
        oflag = 0
        lflag = 0
        cflag = (cflag | termios.CLOCAL | termios.CREAD) & ~termios.HUPCL
        speed = getattr(termios, "B%d" % baud, None)
        if speed is None:
            raise ValueError("unsupported baud rate: %d" % baud)
        termios.tcsetattr(
            fd, termios.TCSANOW, [iflag, oflag, cflag, lflag, speed, speed, cc]
        )
    except Exception:
        os.close(fd)
        raise
    return fd


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="tty device; omit to search by --vid/--pid")
    parser.add_argument("--vid", help="USB vendor id, e.g. 0483")
    parser.add_argument("--pid", help="USB product id, e.g. 5740")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--out", required=True, help="logfile to append to")
    parser.add_argument("--pidfile", help="write our pid here so it can be stopped")
    args = parser.parse_args()

    if not args.port and not (args.vid and args.pid):
        parser.error("give either --port or both --vid and --pid")

    signal.signal(signal.SIGTERM, _stop)
    signal.signal(signal.SIGINT, _stop)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    if args.pidfile:
        with open(args.pidfile, "w") as handle:
            handle.write(str(os.getpid()))

    log = open(args.out, "ab", buffering=0)
    fd = None
    announced_wait = False

    try:
        while _running:
            if fd is None:
                port = args.port or find_tty(args.vid, args.pid)
                if port is None or not os.path.exists(port):
                    if not announced_wait:
                        log.write(b"\n--- waiting for serial device ---\n")
                        announced_wait = True
                    time.sleep(0.5)
                    continue
                try:
                    fd = open_port(port, args.baud)
                except OSError:
                    time.sleep(0.5)
                    continue
                log.write(b"\n--- connected: %s ---\n" % port.encode())
                announced_wait = False

            try:
                chunk = os.read(fd, 4096)
            except OSError as exc:
                if exc.errno in (errno.EAGAIN, errno.EWOULDBLOCK):
                    time.sleep(0.01)
                    continue
                # Device went away (reset, DFU, unplug) - drop back to waiting.
                os.close(fd)
                fd = None
                log.write(b"\n--- device disconnected ---\n")
                continue

            if chunk:
                log.write(chunk)
            else:
                time.sleep(0.01)
    finally:
        if fd is not None:
            os.close(fd)
        log.close()
        if args.pidfile and os.path.exists(args.pidfile):
            os.unlink(args.pidfile)

    return 0


if __name__ == "__main__":
    sys.exit(main())
