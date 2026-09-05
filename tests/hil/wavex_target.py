"""Host-side driver for the boards' debug consoles (tests/hil).

One Target per board. Every call is acknowledged: the driver writes
"WAVEX-DBG <seq> <VERB> ..." to the console port and waits for the line
"WAVEX-DBG: <seq> OK ..." / "... ERR <reason>" carrying its own sequence
number, so a test never sleeps-and-hopes
(docs/features/debug-harness-and-hil.md §3). The grammar itself is pinned
by firmware/shared/tests/debug/.

Transport: the same arrangement scripts/wavex_log.py uses - the port is
written without being claimed for reading, and replies are read from the
serial logger's file (logs/<board>.log, `make logs-start`). That keeps the
loggers running through a HIL run so every board line lands in one place,
and it means the logger must be alive: Target.probe() checks that a PING is
answered *and* that the log file moved, which is the stale-logger symptom
the bench notes warn about.
"""

import os
import re
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO_ROOT, "scripts"))

from serial_ports import DAISY_CDC, ESP32_UART, find_tty  # noqa: E402

BOARDS = {
    "daisy": {"vidpid": DAISY_CDC, "log": "logs/daisy.log"},
    "esp32": {"vidpid": ESP32_UART, "log": "logs/esp32.log"},
}

_ACK = re.compile(rb"WAVEX-DBG: (-?\d+) (OK|ERR)(?: (.*))?$")


class TargetError(Exception):
    pass


class LogTail:
    """Follows a logger's file from its current end; survives rotation."""

    def __init__(self, path):
        self.path = path
        self.fh = open(path, "rb")
        self.fh.seek(0, os.SEEK_END)
        self.ino = os.fstat(self.fh.fileno()).st_ino

    def _reopen_if_rotated(self):
        try:
            st = os.stat(self.path)
        except OSError:
            return
        if st.st_ino != self.ino:
            self.fh.close()
            self.fh = open(self.path, "rb")
            self.ino = st.st_ino

    def lines(self, timeout):
        """Yields complete lines (bytes, stripped) for `timeout` seconds."""
        deadline = time.monotonic() + timeout
        partial = b""
        while time.monotonic() < deadline:
            chunk = self.fh.readline()
            if not chunk:
                self._reopen_if_rotated()
                time.sleep(0.02)
                continue
            if not chunk.endswith(b"\n"):
                partial += chunk
                continue
            line = (partial + chunk).rstrip(b"\r\n")
            partial = b""
            yield line

    def close(self):
        self.fh.close()


class Target:
    def __init__(self, board, run_log=None):
        if board not in BOARDS:
            raise ValueError(board)
        self.board = board
        self.port = find_tty(*BOARDS[board]["vidpid"])
        if self.port is None:
            raise TargetError(f"no {board} on USB")
        self.logfile = os.path.join(REPO_ROOT, BOARDS[board]["log"])
        if not os.path.exists(self.logfile):
            raise TargetError(
                f"{self.logfile} missing - start the serial loggers "
                "(make logs-start)"
            )
        self.tail = LogTail(self.logfile)
        self.seq = int(time.time()) % 100000 * 10
        self.run_log = run_log

    # -- transport ---------------------------------------------------------

    def _write(self, line):
        # Re-resolve on every write: the Daisy re-enumerates on reset.
        port = find_tty(*BOARDS[self.board]["vidpid"]) or self.port
        self.port = port
        fd = os.open(port, os.O_WRONLY | os.O_NOCTTY)
        try:
            os.write(fd, (line + "\n").encode())
        finally:
            os.close(fd)
        if self.run_log:
            self.run_log.write(f"{self.board} > {line}\n")

    def cmd(self, verb, *args, timeout=3.0):
        """Sends one verb; returns the OK dict. Raises on ERR/timeout."""
        self.seq += 1
        seq = self.seq
        line = " ".join(["WAVEX-DBG", str(seq), verb] + [str(a) for a in args])
        self._write(line)
        want = str(seq).encode()
        for raw in self.tail.lines(timeout):
            if self.run_log:
                text = raw.decode(errors="replace")
                self.run_log.write(f"{self.board} < {text}\n")
            m = _ACK.search(raw)
            if not m or m.group(1) != want:
                continue
            body = (m.group(3) or b"").decode(errors="replace")
            if m.group(2) == b"ERR":
                raise TargetError(f"{self.board}: {verb} -> ERR {body}")
            return _parse_kv(body)
        raise TargetError(
            f"{self.board}: no ack for '{line}' within {timeout}s",
        )

    def probe(self):
        """True when the board answers and the logger is actually moving."""
        before = os.stat(self.logfile).st_mtime
        try:
            self.cmd("PING", timeout=2.0)
        except TargetError:
            return False
        return os.stat(self.logfile).st_mtime >= before

    def expect_log(self, pattern, timeout=5.0):
        """Waits for a log line matching `pattern` (regex, str). Returns it."""
        if isinstance(pattern, str):
            pattern = pattern.encode()
        rx = re.compile(pattern)
        for raw in self.tail.lines(timeout):
            if rx.search(raw):
                return raw.decode(errors="replace")
        raise TargetError(
            f"{self.board}: no log line matching {pattern!r} within {timeout}s"
        )

    # -- conveniences ------------------------------------------------------

    def state(self):
        return self.cmd("STATE")

    def wait_state(self, timeout=5.0, **conds):
        """Polls STATE until every key=predicate holds; returns the state.

        A predicate is a callable on the string value, or a plain value that
        must compare equal (as strings).
        """
        deadline = time.monotonic() + timeout
        last = None
        while True:
            last = self.state()
            ok = True
            for key, want in conds.items():
                have = last.get(key)
                if callable(want):
                    ok = ok and have is not None and want(have)
                else:
                    ok = ok and have == str(want)
            if ok:
                return last
            if time.monotonic() > deadline:
                raise TargetError(
                    f"{self.board}: state never met {conds}; last {last}",
                )
            time.sleep(0.1)

    def close(self):
        self.tail.close()


class Esp32(Target):
    def __init__(self, run_log=None):
        super().__init__("esp32", run_log)

    def key(self, name, action="TAP"):
        """Presses a panel key by PanelKey name (SOFT3, SAMPLE, PAD16...)."""
        return self.cmd("KEY", name, action)

    def jump(self, group):
        """Panel jump key to a root group; waits for the stack to follow."""
        self.key(group.upper())
        return self.wait_state(root=group.capitalize(), depth="2")

    def enc(self, delta, steps=True):
        """Turns the encoder by `delta` detents.

        One event per detent by default - a slow human turn - because some
        pages step once per event whatever magnitude it carries. steps=False
        posts a single event carrying the whole magnitude, the way the UI
        task's poll does for a fast spin.
        """
        if not steps:
            return self.cmd("ENC", delta)
        unit = 1 if delta > 0 else -1
        for _ in range(abs(delta)):
            out = self.cmd("ENC", unit)
        return out

    def pot(self, delta, steps=True):
        if not steps:
            return self.cmd("POT", delta)
        unit = 1 if delta > 0 else -1
        for _ in range(abs(delta)):
            out = self.cmd("POT", unit)
        return out

    def tap(self, x, y):
        return self.cmd("TAP", x, y)

    def home(self):
        return self.cmd("HOME")

    def track(self, n):
        return self.cmd("TRACK", n)

    def page(self, *args):
        return self.cmd("PAGE", *args)

    def softkeys(self, state=None):
        """{label: (index, enabled, (x, y))} for the row STATE reports."""
        st = state or self.state()
        keys = {}
        for i in range(6):
            label = st.get(f"sk{i}")
            if not label or label == "-":
                continue
            en = st.get(f"sk{i}en", "1") == "1"
            xy = st.get(f"sk{i}xy")
            pt = tuple(int(v) for v in xy.split(",")) if xy else None
            keys[label.replace("_", " ")] = (i, en, pt)
        return keys

    def softkey(self, label, state=None):
        """Taps the softkey with `label` through the real touch path."""
        keys = self.softkeys(state)
        if label not in keys:
            raise TargetError(
                f"esp32: no softkey {label!r}; have {sorted(keys)}",
            )
        _, enabled, pt = keys[label]
        if not enabled:
            raise TargetError(f"esp32: softkey {label!r} is disabled")
        if pt is None:
            raise TargetError(f"esp32: softkey {label!r} has no geometry")
        return self.tap(*pt)

    def open_menu(self, item):
        """From the main menu, selects `item` by name via the encoder."""
        self.home()
        st = self.wait_state(page="Main_Menu")
        items = ["Sample", "Instrument", "Play", "Settings", "Diagnostics"]
        if item not in items:
            raise TargetError(f"unknown menu item {item!r}")
        # Steer from wherever the highlight was left (the menu wraps, so
        # there is no "go to top" gesture) to the wanted item, by encoder.
        cur = int(st.get("selidx", "0"))
        delta = items.index(item) - cur
        if delta:
            self.enc(delta)
            self.wait_state(sel=item)
        self.key("SELECT")
        return self.wait_state(page=lambda p: p != "Main_Menu")


class Daisy(Target):
    def __init__(self, run_log=None):
        super().__init__("daisy", run_log)

    def tracks(self):
        st = self.cmd("TRACKS")
        return {int(k[1:]): v for k, v in st.items() if k.startswith("t")}

    def routing(self):
        """Each Track's midi_in: {track: 0 Omni | 1..16 that MIDI channel |
        255 Off}. Its own verb because TRACKS plus routing overran the
        console's 256-byte reply line."""
        st = self.cmd("ROUTING")
        return {int(k[1:]): int(v) for k, v in st.items() if k.startswith("m")}

    def samples(self):
        st = self.cmd("SAMPLES")
        ids = st.get("ids", "-")
        return [] if ids == "-" else [int(v) for v in ids.split(",")]

    def note(self, track, note, vel=100, on=True):
        """A Track-addressed note: what the Play grid and sequencer send."""
        state = "ON" if on else "OFF"
        return self.cmd("NOTE", track, note, vel, state, "TRACK")

    def midi_note(self, channel, note, vel=100, on=True):
        """A note on MIDI `channel` (1..16, as displayed) sent the way the
        MIDI task forwards a cable event - the backend routes it to every
        Track whose midi_in matches."""
        state = "ON" if on else "OFF"
        return self.cmd("NOTE", channel, note, vel, state, "MIDI")

    def msg(self, msg_type, payload=b""):
        hex_payload = payload.hex() if payload else ""
        return self.cmd("MSG", f"{msg_type:02x}", hex_payload)

    # Wire messages the tests use for set-up and clean-up (protocol.h).
    MSG_SAMPLE_LOAD = 0x04
    MSG_SAMPLE_SELECT = 0x45
    MSG_SAMPLE_UNLOAD = 0x46
    MSG_TRACK_OP = 0x63

    # TrackOp / TrackMidiIn (protocol.h)
    TRACK_OP_SET_MIDI_IN = 0x01
    MIDI_IN_OMNI = 0
    MIDI_IN_OFF = 0xFF

    def set_midi_in(self, track, midi_in):
        """MSG_TRACK_OP SET_MIDI_IN. `midi_in` is MIDI_IN_OMNI, 1..16 as
        displayed, or MIDI_IN_OFF."""
        op = self.TRACK_OP_SET_MIDI_IN
        payload = struct.pack("<BBH", op, track, midi_in)
        return self.msg(self.MSG_TRACK_OP, payload)

    def reset_routing(self):
        """Back to the default of one channel per Track (Track 1 -> MIDI 1)."""
        for t in range(16):
            self.set_midi_in(t, t + 1)

    def load_sample(self, sample_id, path):
        """MSG_SAMPLE_LOAD: make `path` resident under `sample_id`, without
        going through the frontend's browser. The size/rate/channel/depth
        fields are hints the Daisy re-reads from the file, so 0 is fine."""
        name = path.encode()[:95]
        payload = struct.pack("<HIHBB96s", sample_id, 0, 0, 0, 0, name)
        return self.msg(self.MSG_SAMPLE_LOAD, payload)

    def bind_track(self, track, sample_id, root_note=60):
        """MSG_SAMPLE_SELECT: bind a resident sample to a Track."""
        payload = struct.pack("<HBB", sample_id, track, root_note)
        return self.msg(self.MSG_SAMPLE_SELECT, payload)

    def unbind_track(self, track):
        """MSG_SAMPLE_SELECT with sample_id 0 empties a sample-bound Track."""
        payload = struct.pack("<HBB", 0, track, 0)
        return self.msg(self.MSG_SAMPLE_SELECT, payload)

    def unload_sample(self, sample_id):
        return self.msg(self.MSG_SAMPLE_UNLOAD, struct.pack("<H", sample_id))

    def reset_samples(self):
        """Unbinds every sample-bound Track and unloads every resident WAV, so
        a run leaves the Daisy the way it found it. Instrument-held Tracks
        are left alone (only the load handshake can free an import)."""
        for t, state in self.tracks().items():
            if state.startswith("sample:"):
                self.unbind_track(t)
        for sid in self.samples():
            self.unload_sample(sid)


def _parse_kv(body):
    out = {}
    for tok in body.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            out[k] = v
        else:
            out[tok] = ""
    return out
