#!/usr/bin/env python3
"""Exercise generated application PDUs and concurrent UDS using raw CAN only."""
import argparse
from pathlib import Path
import socket
import struct
import subprocess
import threading
import time

FRAME = struct.Struct("=IB3x8s")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--interface", default="vcan0")
    args = parser.parse_args()
    logs, frames, failures = [], [], []
    changed, stop = threading.Condition(), threading.Event()

    with socket.socket(socket.PF_CAN, socket.SOCK_RAW, socket.CAN_RAW) as bus:
        bus.bind((args.interface,))
        bus.settimeout(0.05)
        process = subprocess.Popen([str(args.executable.resolve()), f"--can-if={args.interface}", "-rt"],
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

        def read_logs():
            for line in process.stdout:
                with changed:
                    logs.append(line.strip())
                    changed.notify_all()

        def read_frames():
            while not stop.is_set():
                try:
                    ident, length, data = FRAME.unpack(bus.recv(FRAME.size))
                    with changed:
                        frames.append((ident, data[:length]))
                        changed.notify_all()
                except TimeoutError:
                    continue
                except OSError as error:
                    if not stop.is_set():
                        failures.append(error)
                    return

        def wait(predicate, timeout=2):
            deadline = time.monotonic() + timeout
            with changed:
                while not predicate():
                    assert not failures, failures
                    assert process.poll() is None, logs[-20:]
                    remaining = deadline - time.monotonic()
                    assert remaining > 0, (logs[-20:], frames[-20:])
                    changed.wait(min(remaining, 0.05))

        def send(ident, data):
            bus.send(FRAME.pack(ident, len(data), data.ljust(8, b"\0")))

        workers = [threading.Thread(target=read_logs, daemon=True),
                   threading.Thread(target=read_frames, daemon=True)]
        for worker in workers:
            worker.start()
        try:
            wait(lambda: "COMM_READY" in logs)
            wait(lambda: (0x220, b"\0\0") in frames and (0x222, b"\0\0\0") in frames)
            assert not any(ident == 0x221 for ident, _ in frames), "EVENT sent without a trigger"
            send(0x120, b"\x01\xf8\x50\0")  # Demand=-123, LedRequest=1
            wait(lambda: "COMMAND -123 1 1 status=0" in logs and "CHANGED 1" in logs)
            wait(lambda: (0x220, b"\x01\x01") in frames and
                 (0x221, b"\xff\x85\x01") in frames and (0x222, b"\x01\0\0") in frames)
            wait(lambda: "VALID 1" in logs)

            # Identical PDU: reception callback still fires, raw-change callback does not.
            send(0x120, b"\x01\xf8\x50\0")
            wait(lambda: "COMMAND -123 1 2 status=0" in logs)
            wait(lambda: (0x220, b"\x01\x02") in frames)
            with changed:
                assert logs.count("CHANGED 1") == 1, logs
            send(0x18ff0101 | socket.CAN_EFF_FLAG, b"\xff\xff\xff\xff\x80\0\0\0")
            wait(lambda: "FEEDBACK 4294967295 -2048" in logs)

            # Neither wrong DLC nor out-of-range Demand can publish a partial PDU.
            send(0x120, b"\x00\0\0")
            send(0x120, b"\x00\x80\0\0")
            send(0x7e0, b"\x02\x3e\0")
            wait(lambda: any(ident == 0x7e8 and data[:3] == b"\x02\x7e\0" for ident, data in frames))
            wait(lambda: "VALID 0" in logs)
            time.sleep(0.05)
            with changed:
                assert sum(line.startswith("COMMAND ") for line in logs) == 2, logs
                assert sum(ident == 0x221 for ident, _ in frames) == 1, frames
                assert logs.count("CHANGED 1") == 1, logs

            send(0x120, b"\x00\x00\xa0\0")  # Demand=10, LedRequest=0
            wait(lambda: "COMMAND 10 0 3 status=0" in logs and "CHANGED 0" in logs)
            wait(lambda: (0x220, b"\0\x03") in frames and (0x221, b"\0\x0a\x03") in frames)
            assert not failures, failures
            print("PASS: generated CAN IDs/bytes, signed/extended RX, PDU vs signal notifications,")
            print("      periodic/event/mixed TX, unchanged suppression, RX bounds/timeout/revalidation,")
            print("      and concurrent UDS TesterPresent without kernel CAN_ISOTP")
        finally:
            stop.set()
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2)
            for worker in workers:
                worker.join(timeout=1)
            process.stdout.close()
            print("\n".join(logs[-20:]))


if __name__ == "__main__":
    main()
