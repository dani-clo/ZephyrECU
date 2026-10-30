#!/usr/bin/env python3
"""Exercise LED SWC persistence through UDS and a fresh process over raw CAN."""
import argparse
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import threading
import time

FRAME = struct.Struct("=IB3x8s")


def exercise(executable, interface, flash_file, expected):
    logs = []
    with socket.socket(socket.PF_CAN, socket.SOCK_RAW, socket.CAN_RAW) as bus:
        bus.bind((interface,))
        bus.settimeout(0.05)
        proc = subprocess.Popen(
            [str(executable), f"--can-if={interface}", "-rt", f"-flash={flash_file}"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )

        def collect():
            logs.extend(line.strip() for line in proc.stdout)

        reader = threading.Thread(target=collect, daemon=True)
        reader.start()
        try:
            deadline = time.monotonic() + 3
            while not any("ZephyrECU CAN and diagnostics ready" in line for line in logs):
                assert proc.poll() is None, logs
                assert time.monotonic() < deadline, logs
                time.sleep(0.01)

            def request(payload):
                wire = bytes([len(payload)]) + payload
                bus.send(FRAME.pack(0x7E0, len(wire), wire.ljust(8, b"\0")))
                deadline = time.monotonic() + 1
                while time.monotonic() < deadline:
                    assert proc.poll() is None, logs
                    try:
                        ident, size, data = FRAME.unpack(bus.recv(FRAME.size))
                    except TimeoutError:
                        continue
                    if ident == 0x7E8:
                        assert 0 < data[0] <= 7 and data[0] < size, (size, data)
                        return data[1:1 + data[0]]
                raise AssertionError(("No UDS response", logs))

            assert request(b"\x10\x03").startswith(b"\x50\x03")
            state = request(b"\x31\x03\xf0\x02")
            assert state == bytes([0x71, 3, 0xF0, 2, expected, 0, 0]), state
            assert request(b"\x31\x01\xf0\x02") == b"\x7f\x31\x13"
            assert request(b"\x31\x01\xf0\x02\x02") == b"\x7f\x31\x31"
            assert request(b"\x31\x02\xf0\x02") == b"\x7f\x31\x12"
            changed = 1 - expected
            state = request(bytes([0x31, 1, 0xF0, 2, changed]))
            assert state[:5] == bytes([0x71, 1, 0xF0, 2, changed]), state
            deadline = time.monotonic() + 2
            while True:
                state = request(b"\x31\x03\xf0\x02")
                assert state[:5] == bytes([0x71, 3, 0xF0, 2, changed]), state
                assert state[6] == 0, state  # explicit SWC write-error status
                if state[5] == 0:
                    break
                assert time.monotonic() < deadline, state
                time.sleep(0.01)
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=2)
            reader.join(timeout=1)
            proc.stdout.close()
        return changed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--interface", default="vcan0")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="zephyrecu-nvm-") as scratch:
        flash = Path(scratch) / "flash.bin"
        first = exercise(args.executable.resolve(), args.interface, flash, 1)
        exercise(args.executable.resolve(), args.interface, flash, first)
    print("PASS: default/config update, UDS input validation, SWC durability status,")
    print("      and persisted LED configuration after a fresh application process")


if __name__ == "__main__":
    main()
