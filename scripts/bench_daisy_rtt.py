#!/usr/bin/env python3
"""Compare identical foreground log bursts received over USB and RTT.

Requires the optional RTT image, an OpenOCD RTT server, and the normal USB
logger. Sends only PING, LOG ?, LOGSTATS and STATE; no musical/file mutations.
Reports complete burst delivery latency (including command handling), not a
USB/SWD line-rate measurement. See docs/logging.md for setup.
"""

import argparse
import json
import select
import socket
import statistics
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests/hil"))
from wavex_target import BOARDS, Daisy  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--usb-log", default="logs/daisy.log")
    parser.add_argument("--rtt-port", type=int, default=9090)
    parser.add_argument("--bursts", type=int, default=20)
    args = parser.parse_args()
    if not 1 <= args.bursts <= 1000:
        parser.error("bursts must be 1..1000")
    BOARDS["daisy"]["log"] = args.usb_log
    target = Daisy()
    try:
        address = ("localhost", args.rtt_port)
        with socket.create_connection(address, timeout=3) as rtt:
            rtt.setblocking(False)
            # Opening TCP does not instantly empty an RTT ring accumulated
            # without a reader. Wait for a fresh ACK through RTT before timing.
            deadline = time.monotonic() + 5
            warm = False
            while not warm and time.monotonic() < deadline:
                target.cmd("PING")
                marker = f"WAVEX-DBG: {target.seq} OK\r\n".encode()
                warmup = bytearray()
                attempt_end = time.monotonic() + 0.25
                while time.monotonic() < attempt_end:
                    if select.select([rtt], [], [], 0.01)[0]:
                        chunk = rtt.recv(65536)
                        if not chunk:
                            raise RuntimeError("RTT disconnected")
                        warmup += chunk
                        if marker in warmup:
                            warm = True
                            break
            if not warm:
                raise RuntimeError("RTT warm-up acknowledgement timed out")
            before = target.cmd("LOGSTATS")
            if "rtt_drop" not in before:
                raise RuntimeError("image has no RTT loss counters")
            state_before = target.state()
            rows = []
            base_seq = time.monotonic_ns() % 90000000
            with open(ROOT / args.usb_log, "rb", buffering=0) as usb:
                for burst in range(args.bursts):
                    # Discard preceding boot/telemetry; own sequence markers
                    # delimit the same complete records on both transports.
                    while select.select([rtt], [], [], 0)[0]:
                        if not rtt.recv(65536):
                            raise RuntimeError("RTT disconnected")
                    usb.seek(0, 2)
                    seq = base_seq + burst
                    # Daisy accepts one pending console line, so each burst
                    # is one LOG ? request (all module levels plus its ACK).
                    start_marker = b"WAVEX-LOG: "
                    end_marker = f"WAVEX-DBG: {seq} OK\r\n".encode()
                    buffers = {"usb": bytearray(), "rtt": bytearray()}
                    done = {}
                    started = time.monotonic()
                    target._write(f"WAVEX-DBG {seq} LOG ?")
                    while len(done) < 2 and time.monotonic() - started < 5:
                        if select.select([rtt], [], [], 0.001)[0]:
                            chunk = rtt.recv(65536)
                            if not chunk:
                                raise RuntimeError("RTT disconnected")
                            buffers["rtt"] += chunk
                        buffers["usb"] += usb.read(65536)
                        for name, buf in buffers.items():
                            if name in done:
                                continue
                            start = buf.find(start_marker)
                            end = buf.find(end_marker, max(start, 0))
                            if start >= 0 and end >= 0:
                                stop = end + len(end_marker)
                                payload = bytes(buf[start:stop])
                                elapsed = time.monotonic() - started
                                done[name] = (elapsed, payload)
                    if len(done) != 2:
                        print(
                            json.dumps(
                                {
                                    "before": before,
                                    "after": target.cmd("LOGSTATS"),
                                    "completed_bursts": rows,
                                    "failed_burst": burst,
                                    "received": {
                                        k: v.hex() for k, v in buffers.items()
                                    },
                                },
                                indent=2,
                            )
                        )
                        raise RuntimeError(
                            f"burst {burst}: incomplete delivery {list(done)}"
                        )
                    if done["usb"][1] != done["rtt"][1]:
                        print(
                            json.dumps(
                                {
                                    "before": before,
                                    "after": target.cmd("LOGSTATS"),
                                    "completed_bursts": rows,
                                    "failed_burst": burst,
                                    "received": {
                                        k: v[1].hex() for k, v in done.items()
                                    },
                                },
                                indent=2,
                            )
                        )
                        raise RuntimeError(f"burst {burst}: payload mismatch")
                    rows.append(
                        {
                            "bytes": len(done["usb"][1]),
                            "usb_ms": round(done["usb"][0] * 1000, 3),
                            "rtt_ms": round(done["rtt"][0] * 1000, 3),
                        }
                    )
            after = target.cmd("LOGSTATS")
            state_after = target.state()
            result = {
                "before": before,
                "after": after,
                "state_before": state_before,
                "state_after": state_after,
                "bursts": rows,
                "median_usb_ms": statistics.median(r["usb_ms"] for r in rows),
                "median_rtt_ms": statistics.median(r["rtt_ms"] for r in rows),
            }
            print(json.dumps(result, indent=2))
            for name in ("usb_drop", "rtt_drop", "isr_log"):
                if after[name] != before[name]:
                    raise RuntimeError(f"{name} increased during capture")
            if state_after["underruns"] != state_before["underruns"]:
                raise RuntimeError("audio underrun count increased")
    finally:
        target.close()


if __name__ == "__main__":
    main()
