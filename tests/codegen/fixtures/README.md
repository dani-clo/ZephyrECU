# DBC generator fixtures and compiled codec tests

These DBCs and optional YAML policies are test inputs, separate from the
application's `dbc/` directory. `valid/` and `invalid/` retain paired YAML inputs;
`dbc_only/` exercises generation with `--node ECU` and no YAML. The original
fixtures were reviewed with the pinned cantools version: all three valid inputs are accepted and all sixteen
invalid inputs are rejected as intended.

## Run locally

From the repository root, with Python and a host GCC-compatible C compiler:

```sh
python -m pip install -r tools/requirements-codegen.txt
python -m unittest discover -s tests/codegen -v
```

Run just the fixtures, also from another working directory:

```sh
python /path/to/ZephyrECU/tests/codegen/test_fixtures.py --repo /path/to/ZephyrECU -v
```

`test_fixtures.py` uses the adjacent `codec_oracle.py` helper and `fixtures/`
directory. `--repo` selects the generator and component headers to test. Set
`CC=clang` (or another GCC-compatible compiler command) to override `gcc`.
The compiled oracle runs on Linux; it needs no Zephyr SDK, CAN device or west.
Temporary generated sources and shared libraries are removed automatically.

CI runs unittest discovery in its own `codegen` job, without waiting for Zephyr
setup. A compiler error, unexpected rejection/acceptance, stale committed output,
or codec mismatch fails the job; its log is uploaded even on failure.

## Expected results

| Fixture | Expected | Coverage |
| --- | --- | --- |
| valid/single_message | ACCEPT | Minimal fixture, PERIODIC TX |
| valid/limit_32_messages_128_signals | ACCEPT | Global maxima: 32 PDUs, 128 signals; RX/TX |
| valid/bit_layouts_and_extended | ACCEPT | Intel/Motorola, signed, cross-byte, extended IDs, MIXED/EVENT TX and triggers |
| valid/all_widths_signed_intel | ACCEPT | All widths 1..32: signed Intel and unsigned Motorola |
| valid/all_widths_signed_motorola | ACCEPT | All widths 1..32: unsigned Intel and signed Motorola |
| valid/rounded_bounds | ACCEPT | Fractional scale/offset, inward-rounded raw limits, singleton range |
| valid/dlc1_and_16_signals | ACCEPT | DLC 1, maximum 16 signals/PDU, same numeric standard/extended ID |
| invalid/over_limit_33_messages, over_limit_100_messages | REJECT | 32-PDU limit and stress rejection |
| invalid/over_limit_129_signals, over_limit_17_signals | REJECT | Global and per-PDU signal limits |
| invalid/syntax_* | REJECT | Malformed DBC syntax |
| invalid/overlap_signals, outside_dlc, zero_length_signal | REJECT | Invalid signal layouts |
| invalid/negative_scale, unsupported_* | REJECT | Unsupported scale, multiplexing, floats, DLC 9, width 33 |
| invalid/missing_policy_pdu, duplicate_policy_key, bad_timeout | REJECT | Required RX timeout absent, duplicate keys, invalid timing |
| invalid/invalid_identifier | REJECT | Illegal DBC name, rejected by cantools syntax parsing |
| invalid/empty_bounds | REJECT | Physical interval containing no representable raw value |
| invalid/unknown_node, wrong_receiver | REJECT | Node declaration and RX routing |
| invalid/duplicate_route, normalized_signal_collision | REJECT | Duplicate CAN route and normalized C signal name |

The `dbc_only/minimal_dbc_only` and `dbc_only/attributes_override` examples are
accepted without YAML and round-tripped through compiled C. See
[CAN configuration](../../../docs/can-comm.md#dbc-only-configuration-and-yaml-overrides)
for defaults and supported attributes. `test_optional_policy.py` covers partial
overrides, attribute defaults/enums, node selection, malformed/unknown policy
keys, timing conflicts, CLI read-only checks and unchanged demo output.

Negative fixtures require a relevant diagnostic, not merely any exception.
Missing files and unexpected Python errors fail the test rather than counting
as expected rejection. Inventory checks detect unlisted negative fixtures and
DBC/policy pairing mistakes. The legacy `missing_policy_pdu` fixture still fails
because its RX timeout cannot be derived; omission of an override is otherwise
permitted.

## Codec oracle

For **every valid fixture and the application DBC**, tests generate all four C
files, check deterministic output, and compile both C translation units using
C17 and `-Wall -Wextra -Werror`. Only the communication API calls required for
linking are stubbed; the actual generated codec and configuration are executed.

A separate cantools database supplies message definitions, raw limits and the
reference encoder/decoder. The oracle checks the complete signal inventory.
For every message it exercises the minimum, maximum, clamped 0/-1/+1, and 100
seeded random raw vectors (`Random(42)`). Raw limits are derived from physical
DBC bounds and representable bit ranges, with inward rounding.

Every vector is encoded by generated C into zero-filled, one-filled and random
8-byte buffers. All bytes are checked against cantools, including preservation
of unused bits and bytes past DLC. Generated C decodes the result and must match
both cantools and the original values for every signal. This covers clearing
bits as well as setting them, signed extension, and both byte orders. Choices
and physical scaling are disabled at the codec boundary, which exposes raw
integers. Null pointers and invalid signal IDs are also exercised.
