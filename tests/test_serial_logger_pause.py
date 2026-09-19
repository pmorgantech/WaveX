"""Flash/monitor commands must not race the USB console reader."""

import os
import pty
import signal
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WRAPPER = ROOT / "scripts/with_serial_logger_paused.py"
LOGGER = ROOT / "scripts/serial_log.py"


def wait_for(predicate):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.02)
    raise AssertionError("condition did not become true")


def test_failed_command_resumes_logger_without_rotating_output(tmp_path):
    master, slave = pty.openpty()
    logfile = tmp_path / "serial.log"
    pidfile = tmp_path / "serial.pid"
    original = subprocess.Popen(
        [
            sys.executable,
            str(LOGGER),
            "--port",
            os.ttyname(slave),
            "--out",
            str(logfile),
            "--pidfile",
            str(pidfile),
        ],
        cwd=ROOT,
    )
    try:
        wait_for(logfile.exists)
        wait_for(lambda: b"connected:" in logfile.read_bytes())
        os.write(master, b"before\n")
        wait_for(lambda: b"before\n" in logfile.read_bytes())
        result = subprocess.run(
            [
                sys.executable,
                str(WRAPPER),
                "--pidfile",
                str(pidfile),
                "--",
                sys.executable,
                "-c",
                "raise SystemExit(7)",
            ],
            cwd=tmp_path,
            timeout=10,
        )
        assert result.returncode == 7
        wait_for(pidfile.exists)
        wait_for(lambda: pidfile.read_text() != str(original.pid))
        wait_for(lambda: logfile.read_bytes().count(b"connected:") == 2)
        os.write(master, b"after\n")
        wait_for(lambda: b"after\n" in logfile.read_bytes())
        assert b"before\n" in logfile.read_bytes()
        assert original.wait(timeout=3) == 0
    finally:
        if pidfile.exists():
            try:
                os.kill(int(pidfile.read_text()), signal.SIGTERM)
            except ProcessLookupError:
                pass
        if original.poll() is None:
            original.terminate()
            original.wait(timeout=3)
        os.close(master)
        os.close(slave)


def test_no_logger_preserves_command_exit_status(tmp_path):
    result = subprocess.run(
        [
            sys.executable,
            str(WRAPPER),
            "--pidfile",
            str(tmp_path / "missing"),
            "--",
            sys.executable,
            "-c",
            "raise SystemExit(9)",
        ],
        timeout=5,
    )
    assert result.returncode == 9


def test_reused_pid_is_not_signalled(tmp_path):
    pidfile = tmp_path / "stale.pid"
    pidfile.write_text(str(os.getpid()))
    result = subprocess.run(
        [
            sys.executable,
            str(WRAPPER),
            "--pidfile",
            str(pidfile),
            "--",
            sys.executable,
            "-c",
            "raise SystemExit(0)",
        ],
        capture_output=True,
        timeout=5,
    )
    assert result.returncode != 0
    assert b"refusing to stop" in result.stderr
