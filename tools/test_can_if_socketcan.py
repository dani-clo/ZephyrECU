#!/usr/bin/env python3
"""Exercise the real can_if + native SocketCAN driver; no external Python packages."""
import argparse
import re
import socket
import struct
import subprocess
import threading
import time
from pathlib import Path

CAN_EFF_FLAG = 0x80000000
CAN_RTR_FLAG = 0x40000000
CAN_FRAME = struct.Struct("=IB3x8s")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--interface", default="vcan0")
    args = parser.parse_args()
    lines = []
    changed = threading.Condition()

    with socket.socket(socket.PF_CAN, socket.SOCK_RAW, socket.CAN_RAW) as bus:
        bus.bind((args.interface,))
        bus.settimeout(2)
        proc = subprocess.Popen(
            [str(args.executable.resolve()), f"--can-if={args.interface}", "-rt"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )

        def collect():
            for line in proc.stdout:
                with changed:
                    lines.append(line.strip())
                    changed.notify_all()

        reader = threading.Thread(target=collect, daemon=True)
        reader.start()

        def wait_line(predicate, start=0, timeout=3):
            deadline = time.monotonic() + timeout
            with changed:
                while True:
                    for line in lines[start:]:
                        if predicate(line):
                            return line
                    remaining = deadline - time.monotonic()
                    if remaining <= 0 or proc.poll() is not None:
                        raise AssertionError(f"Expected log missing; recent logs: {lines[-15:]}")
                    changed.wait(min(remaining, 0.1))

        def send(can_id, data):
            bus.send(CAN_FRAME.pack(can_id, len(data), data.ljust(8, b"\0")))

        def echo(can_id, payload, response_id):
            send(can_id, payload)
            raw = bus.recv(CAN_FRAME.size)
            ident, length, data = CAN_FRAME.unpack(raw)
            assert (ident, length, data[:length]) == (response_id, len(payload), payload)

        try:
            wait_line(lambda line: line == "READY")
            # Unique payloads make order and duplicate dispatch errors visible.
            for value in range(10):
                echo(0x123, bytes([value, 0x55, 0xAA]), 0x321)
            echo(CAN_EFF_FLAG | 0x123, b"\xEE", CAN_EFF_FLAG | 0x1ABCDE)
            wait_line(lambda line: "completed=11 " in line and "pending=0" in line)
            assert len([s for s in lines if s.startswith("RX_A 123 0")]) == 10
            assert len([s for s in lines if s.startswith("RX_B 123 0")]) == 10
            assert len([s for s in lines if s.startswith("RX_A 123 1")]) == 1
            assert not any(s.startswith("RX_B 123 1") for s in lines)
            # Unmatched IDs (including same base ID with wrong frame type) and RTR
            # must not produce callbacks or echoes.
            start = len(lines)
            send(0x125, b"\xFA")
            send(CAN_EFF_FLAG | 0x124, b"\xFB")
            send(CAN_RTR_FLAG | 0x123, b"")
            wait_line(lambda line: line.startswith("STATS "), start)
            assert not any(s.startswith("RX_") or s == "BLOCKED" for s in lines[start:])
            bus.settimeout(0.1)
            try:
                bus.recv(CAN_FRAME.size)
                raise AssertionError("Unmatched/RTR frame caused unexpected TX")
            except TimeoutError:
                pass
            bus.settimeout(2)
            # Deterministic backpressure: stop dispatcher in test-only callback.
            start = len(lines)
            send(0x124, b"\xF0")
            wait_line(lambda line: line == "BLOCKED", start)
            for value in range(96):
                send(0x123, bytes([value]))
            wait_line(lambda line: line == "UNBLOCKED", start)
            summary = wait_line(
                lambda line: line.startswith("STATS ") and
                "rx=44 " in line and "submitted=43 " in line and
                "completed=43 " in line and "pending=0" in line, start,
            )
            assert int(re.search(r"dropped=(\d+)", summary)[1]) == 64
            assert "errors=0 " in summary
            assert len([s for s in lines if s.startswith("RX_A 123 0")]) == 42
            assert len([s for s in lines if s.startswith("RX_B 123 0")]) == 42
            assert not any("TEST_ECHO_QUEUE_FULL" in line or "TX_FAILED" in line
                           for line in lines)
            print("PASS: exact standard/extended filtering, fanout exactly once, RX payloads,")
            print("      ordered TX payloads/completions, unmatched/RTR rejection and RX overflow")
            print(summary)
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            reader.join(timeout=1)
            print("\n".join(lines[-5:]))


if __name__ == "__main__":
    main()
