#!/usr/bin/env python3
"""Give a command exclusive use of a managed serial logger's port.

Resume the same logger, appending to the same file, even if the command fails.
Only the process named by the pidfile is touched; reject stale/reused PIDs.
Run inside the same devcontainer/PID namespace as the logger.
"""

import argparse
import os
import signal
import subprocess
import time
from pathlib import Path


def pause(pidfile):
    try:
        pid = int(pidfile.read_text().strip())
    except FileNotFoundError:
        return None
    if pid <= 1:
        raise RuntimeError(f"invalid logger PID in {pidfile}")
    proc = Path(f"/proc/{pid}")
    try:
        args = proc.joinpath("cmdline").read_bytes().rstrip(b"\0").split(b"\0")
        args = [os.fsdecode(arg) for arg in args]
        cwd = proc.joinpath("cwd").resolve(strict=True)
    except FileNotFoundError:
        return None
    if len(args) < 2 or Path(args[1]).name != "serial_log.py":
        raise RuntimeError(f"PID {pid} is not a logger; refusing to stop")
    if "--pidfile" not in args:
        raise RuntimeError("logger has no matching pidfile")
    owned = Path(args[args.index("--pidfile") + 1])
    if not owned.is_absolute():
        owned = cwd / owned
    if owned.resolve() != pidfile:
        raise RuntimeError("logger pidfile does not match")
    os.kill(pid, signal.SIGTERM)
    deadline = time.monotonic() + 3
    while proc.exists():
        try:
            state = proc.joinpath("stat").read_text().split(")", 1)[1]
            if state.split()[0] == "Z":
                break
        except FileNotFoundError:
            break
        if time.monotonic() >= deadline:
            raise RuntimeError(f"logger {pid} did not exit; refusing")
        time.sleep(0.05)
    return args, cwd


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pidfile", required=True, type=Path)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command
    if command[:1] == ["--"]:
        command = command[1:]
    if not command:
        parser.error("a command is required after --")
    logger = pause(args.pidfile.resolve())
    try:
        return subprocess.call(command)
    finally:
        if logger:
            argv, cwd = logger
            subprocess.Popen(
                argv,
                cwd=cwd,
                start_new_session=True,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )


if __name__ == "__main__":
    raise SystemExit(main())
