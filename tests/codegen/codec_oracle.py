"""Compile generated C and compare raw signal codecs with independent cantools data."""
import ctypes
from decimal import Decimal, ROUND_CEILING, ROUND_FLOOR
import os
from pathlib import Path
import random
import shlex
import subprocess
import tempfile

import cantools


def raw_bounds(signal):
    """Use DBC physical limits, including inward rounding to representable raw values."""
    scale, offset = Decimal(str(signal.scale)), Decimal(str(signal.offset))
    low = int(((Decimal(str(signal.minimum)) - offset) / scale).to_integral_value(rounding=ROUND_CEILING))
    high = int(((Decimal(str(signal.maximum)) - offset) / scale).to_integral_value(rounding=ROUND_FLOOR))
    return (max(low, -(1 << (signal.length - 1)) if signal.is_signed else 0),
            min(high, (1 << (signal.length - int(signal.is_signed))) - 1))


def check_codec(test, root, codegen, dbc, policy):
    pdus, signals = codegen.model(dbc, policy)
    files = codegen.generate(dbc, policy)
    test.assertEqual(files, codegen.generate(dbc, policy), "generation must be deterministic")
    # Load separately: do not take the reference messages/bounds from the generator model.
    reference = cantools.database.load_file(dbc, database_format="dbc", strict=True)
    test.assertEqual({p["message"].name for p in pdus}, {m.name for m in reference.messages})
    test.assertEqual(len(signals), sum(len(m.signals) for m in reference.messages))
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory)
        for name, content in files.items():
            (output / name).write_text(content, encoding="utf-8")
        (output / "stub.c").write_text('''#include "can_comm.h"
int can_comm_get_signal(can_signal_id_t s, can_comm_sample_t *v)
{ (void)s; (void)v; return -1; }
int can_comm_set_signal_raw(can_signal_id_t s, int64_t v)
{ (void)s; (void)v; return -1; }
''', encoding="utf-8")
        library = output / "codec.so"
        subprocess.run([*shlex.split(os.environ.get("CC", "gcc")), "-std=c17", "-Wall", "-Wextra", "-Werror",
                        "-shared", "-fPIC", "-I", str(output),
                        "-I", str(root / "components/can_comm/include"),
                        str(output / "can_cfg.c"), str(output / "can_signals.c"),
                        str(output / "stub.c"), "-o", str(library)], check=True)
        codec = ctypes.CDLL(str(library))
        buffer_type = ctypes.c_uint8 * 8
        codec.can_signal_decode.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_uint8)]
        codec.can_signal_decode.restype = ctypes.c_int64
        codec.can_signal_encode.argtypes = [ctypes.c_int, ctypes.c_int64, ctypes.POINTER(ctypes.c_uint8)]
        codec.can_signal_encode.restype = None
        untouched = buffer_type(*([0xa5] * 8))
        for index in (-1, len(signals)):
            test.assertEqual(codec.can_signal_decode(index, untouched), 0)
            codec.can_signal_encode(index, 7, untouched)
            test.assertEqual(bytes(untouched), b"\xa5" * 8)
        test.assertEqual(codec.can_signal_decode(0, None), 0)
        codec.can_signal_encode(0, 0, None)
        rng = random.Random(42)
        for pdu in pdus:
            message = reference.get_message_by_name(pdu["message"].name)
            message.unused_bit_pattern = 0xff
            group = list(enumerate(signals))[pdu["first"]:pdu["first"] + pdu["count"]]
            test.assertEqual({s["signal"].name for _, s in group}, {s.name for s in message.signals})
            bounds = {s.name: raw_bounds(s) for s in message.signals}
            for iteration in range(105):
                values = {}
                for name, (low, high) in bounds.items():
                    values[name] = (low if iteration == 0 else high if iteration == 1 else
                                    min(max((0, -1, 1)[iteration - 2], low), high) if iteration < 5 else
                                    rng.randint(low, high))
                with test.subTest(dbc=dbc.name, pdu=message.name, vector=iteration):
                    expected = message.encode(values, scaling=False, padding=False)
                    padded = message.encode(values, scaling=False, padding=True)
                    # Exercise setting AND clearing bits, while retaining padding and bytes past DLC.
                    for fill in (b"\0" * 8, b"\xff" * 8, rng.randbytes(8)):
                        buffer = buffer_type(*fill)
                        for index, signal in group:
                            codec.can_signal_encode(index, values[signal["signal"].name], buffer)
                        mask = bytes(a ^ b for a, b in zip(expected, padded))
                        preserved = bytes(a | (b & c) for a, b, c in zip(expected, fill, mask))
                        test.assertEqual(bytes(buffer), preserved + fill[message.length:])
                        decoded = message.decode(bytes(buffer)[:message.length], scaling=False, decode_choices=False)
                        for index, signal in group:
                            name = signal["signal"].name
                            test.assertEqual(codec.can_signal_decode(index, buffer), decoded[name])
                            test.assertEqual(decoded[name], values[name])
    return len(pdus), len(signals)
