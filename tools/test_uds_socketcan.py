#!/usr/bin/env python3
"""Test the real UDS server with Linux kernel ISO-TP and concurrent raw CAN traffic."""
import argparse
import re
import socket
import struct
import subprocess
import threading
import time
from pathlib import Path

FRAME = struct.Struct("=IB3x8s")
IDENTITY = b"ZephyrECU-UDS-01"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--interface", default="vcan0")
    parser.add_argument("--timing", choices=("strict", "report"), default="strict",
                        help="enforce host latency thresholds (default), or report them for shared CI runners")
    args = parser.parse_args()
    logs, frames, failures, latencies = [], [], [], []
    immediate_latencies = []
    timing_violations = []
    changed = threading.Condition()
    stop = threading.Event()
    raw = socket.socket(socket.PF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
    raw.bind((args.interface,))
    raw.settimeout(0.05)
    timestamp_option = getattr(socket, "SO_TIMESTAMPNS", 35)
    raw.setsockopt(socket.SOL_SOCKET, timestamp_option, 1)
    passed = False
    proc = subprocess.Popen([str(args.executable.resolve()), f"--can-if={args.interface}", "-rt"],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    def collect_logs():
        for line in proc.stdout:
            with changed:
                logs.append(line.strip())
                changed.notify_all()

    def collect_frames():
        while not stop.is_set():
            try:
                packet, ancillary, _, _ = raw.recvmsg(FRAME.size, socket.CMSG_SPACE(16))
                ident, length, payload = FRAME.unpack(packet)
                stamp = next(value for level, kind, value in ancillary
                             if level == socket.SOL_SOCKET and kind == timestamp_option)
                seconds, nanoseconds = struct.unpack("=qq", stamp[:16])
                # Kernel receive time, translated to the monotonic clock. Reader
                # scheduling delay must not be counted as ECU response latency.
                received = time.monotonic() + (seconds * 1_000_000_000 + nanoseconds -
                                               time.time_ns()) / 1_000_000_000
                with changed:
                    frames.append((received, ident, payload[:length]))
                    changed.notify_all()
            except TimeoutError:
                pass
            except OSError as error:
                if not stop.is_set():
                    failures.append(error)
                return

    def wait_for(predicate, timeout=2):
        deadline = time.monotonic() + timeout
        with changed:
            while not predicate():
                assert proc.poll() is None, logs[-10:]
                remaining = deadline - time.monotonic()
                assert remaining > 0, logs[-10:]
                changed.wait(min(remaining, 0.05))

    def send_raw(ident, payload):
        raw.send(FRAME.pack(ident, len(payload), payload.ljust(8, b"\0")))

    def open_tester():
        tester = socket.socket(socket.PF_CAN, socket.SOCK_DGRAM, socket.CAN_ISOTP)
        tester.setsockopt(106, 2, struct.pack("=BBB", 4, 0, 0))  # BS=4, STmin=0
        tester.bind((args.interface, 0x7E8, 0x7E0))
        tester.settimeout(1)
        return tester

    tester = open_tester()
    workers = [threading.Thread(target=collect_logs, daemon=True),
               threading.Thread(target=collect_frames, daemon=True)]
    for thread in workers:
        thread.start()

    def check_timing(condition, message):
        if not condition:
            if args.timing == "strict":
                raise AssertionError(message)
            timing_violations.append(message)

    def exchange(request, expected, *, immediate=False):
        start = time.monotonic()
        tester.send(request)
        reply = tester.recv(4095)
        assert reply == expected, (request.hex(), reply.hex(), expected.hex())
        wait_for(lambda: any(t >= start and ident == 0x7E8 and data and data[0] >> 4 in (0, 1)
                             for t, ident, data in frames))
        first = next(t for t, ident, data in frames
                     if t >= start and ident == 0x7E8 and data and data[0] >> 4 in (0, 1))
        latencies.append(first - start)
        check_timing(first - start <= 0.050,
                     f"P2 exceeded: {(first - start) * 1000:.1f} ms")
        if immediate:
            immediate_latencies.append(first - start)
            check_timing(first - start <= 0.010,
                         f"Immediate response delayed: {(first - start) * 1000:.1f} ms")

    def silence(request, functional=False):
        start = time.monotonic()
        if functional:
            send_raw(0x7DF, bytes([len(request)]) + request)
        else:
            tester.send(request)
        time.sleep(0.12)
        assert not any(t >= start and ident == 0x7E8 for t, ident, _ in frames), request.hex()

    def app_traffic():
        for value in range(256):
            if stop.is_set():
                return
            try:
                send_raw(0x123, bytes([value]))
            except OSError as error:
                failures.append(error)
                return
            stop.wait(0.02)

    try:
        wait_for(lambda: "READY" in logs)
        traffic = threading.Thread(target=app_traffic, daemon=True)
        traffic.start()
        workers.append(traffic)
        # Repeated short requests catch unnecessary inter-response pacing.
        for _ in range(5):
            exchange(b"\x3E\x00", b"\x7E\x00", immediate=True)
        exchange(b"\x10\x03", b"\x50\x03\x00\x32\x01\xF4")
        exchange(b"\x22\xF1\x86", b"\x62\xF1\x86\x03")
        exchange(b"\x10\x02", b"\x7F\x10\x12")
        exchange(b"\x22\xF1\x90", b"\x62\xF1\x90" + IDENTITY)
        # Both request and response are multi-frame, with sequence/block handling.
        exchange(b"\x22" + b"\xF1\x86" * 12, b"\x62" + b"\xF1\x86\x03" * 12)
        # Long response exercises CF sequence-number wrap and repeated FC blocks.
        exchange(b"\x22" + b"\xF1\x90" * 16,
                 b"\x62" + (b"\xF1\x90" + IDENTITY) * 16)
        exchange(b"\x22" + b"\xF1\x86" * 33, b"\x7F\x22\x13")
        exchange(b"\x31\x01\xF0\x01", b"\x71\x01\xF0\x01\x01")
        exchange(b"\x31\x03\xF0\x01", b"\x71\x03\xF0\x01\x01")
        exchange(b"\x31\x02\xF0\x01", b"\x7F\x31\x12")
        exchange(b"\x31\x01\xF0\x01" + b"\x00" * 8, b"\x7F\x31\x13")
        exchange(b"\x31\x01\xFF\xFF", b"\x7F\x31\x31")
        exchange(b"\x22\xFF\xFF", b"\x7F\x22\x31")
        for request in (b"\x10", b"\x10\x01\x00", b"\x22", b"\x22\xF1", b"\x31\x01", b"\x3E"):
            exchange(request, bytes([0x7F, request[0], 0x13]))
        exchange(b"\x3E\x01", b"\x7F\x3E\x12")
        for sid in (0x11, 0x14, 0x19, 0x23, 0x27, 0x28, 0x2E, 0x34, 0x35, 0x36, 0x37, 0x38, 0x3D, 0x85, 0x99):
            exchange(bytes([sid]), bytes([0x7F, sid, 0x11]))
        silence(b"\x3E\x80")
        silence(b"\x99", functional=True)
        silence(b"\x22\xFF\xFF", functional=True)
        silence(b"\x31\x01\xF0\x01", functional=True)
        exchange(b"\x31\x03\xF0\x01", b"\x71\x03\xF0\x01\x01")
        send_raw(0x7DF, b"\x02\x3E\x00")
        assert tester.recv(4095) == b"\x7E\x00"
        send_raw(0x7DF, b"\x03\x22\xF1\x90")
        assert tester.recv(4095) == b"\x62\xF1\x90" + IDENTITY
        # Malformed and segmented functional requests must never solicit FC.
        for ident, payload in ((0x7DF, b"\x10\x09\x22\xF1\x86\xF1\x86\xF1"),
                               (0x7E0, b"\x07\x22"), (0x7E0, b"\x00\x00"),
                               (0x7E0, b"\x40\x00"), (0x7E0, b"\x30\x00")):
            start = time.monotonic()
            send_raw(ident, payload)
            time.sleep(0.12)
            assert not any(t >= start and cid == 0x7E8 for t, cid, _ in frames)
        # Stop kernel automatic FC while testing peer failures and recovery.
        tester.close()
        for corrupt in (True, False):
            start = time.monotonic()
            send_raw(0x7E0, b"\x10\x09\x22\xF1\x86\xF1\x86\xF1")
            wait_for(lambda: any(t >= start and cid == 0x7E8 and data[0] >> 4 == 3
                                 for t, cid, data in frames))
            if corrupt:
                send_raw(0x7E0, b"\x22\x86\xF1\x86\x00\x00\x00\x00")
            time.sleep(0.15)
        start = time.monotonic()
        send_raw(0x7E0, b"\x03\x22\xF1\x90")
        wait_for(lambda: any(t >= start and cid == 0x7E8 and data[0] >> 4 == 1
                             for t, cid, data in frames))
        time.sleep(0.15)  # No FC: ECU must time out, then recover for next request.
        tester = open_tester()
        exchange(b"\x3E\x00", b"\x7E\x00")
        # TesterPresent with suppressed positive response refreshes S3.
        exchange(b"\x10\x03", b"\x50\x03\x00\x32\x01\xF4")
        time.sleep(2.7)
        silence(b"\x3E\x80")
        time.sleep(2.7)
        exchange(b"\x22\xF1\x86", b"\x62\xF1\x86\x03")
        time.sleep(5.2)
        exchange(b"\x22\xF1\x86", b"\x62\xF1\x86\x01")
        wait_for(lambda: any(line.startswith("DIAG ") and "session=1 " in line and
                             "timeouts=1 " in line for line in logs))
        summary = next(line for line in reversed(logs) if line.startswith("DIAG "))
        values = {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", summary)}
        assert values["invalid"] >= 5 and values["transport_errors"] >= 3, summary
        assert values["dropped"] == values["can_drops"] == values["tx_errors"] == 0, summary
        check_timing(values["max_poll_us"] < 5000 and values["poll_misses"] == 0,
                     f"Worker scheduling gaps: max={values['max_poll_us']} us, "
                     f"misses={values['poll_misses']}")
        assert not failures and not any("APP_TX_FAILED" in line for line in logs), failures
        echoes = [data[0] for _, cid, data in frames if cid == 0x321]
        assert echoes == list(range(256)), (len(echoes), echoes[-5:])
        assert any(cid == 0x322 for _, cid, _ in frames)
        print("PASS: UDS services/NRCs, physical/functional policy, single/multi-frame,")
        print("      malformed requests, transport timeout recovery, S3/TesterPresent and concurrent CAN")
        print(f"Maximum measured P2 first-frame latency: {max(latencies) * 1000:.2f} ms")
        print(f"Maximum immediate-response latency: {max(immediate_latencies) * 1000:.2f} ms")
        print(f"Timing policy: {args.timing}")
        for violation in timing_violations:
            print(f"TIMING observation (not a functional failure): {violation}")
        if args.timing == "report":
            print("Host timing is reported only; deterministic pacing regression remains in ztest.")
        print(summary)
        passed = True
    finally:
        stop.set()
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        for thread in workers:
            thread.join(timeout=1)
        tester.close()
        raw.close()
        if not passed or proc.returncode not in (0, -15):
            print("\n".join(logs[-10:]))


if __name__ == "__main__":
    main()
