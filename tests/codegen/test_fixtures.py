#!/usr/bin/env python3
"""Validate DBC/policy fixtures and round-trip all valid signals through compiled C.

Standalone: python tests/codegen/test_fixtures.py [--repo /path/to/ZephyrECU] -v
Also collected by unittest discovery in tests/codegen.
"""
import argparse
import importlib.util
from pathlib import Path
import sys
import unittest

import cantools
import yaml

from codec_oracle import check_codec

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = Path(__file__).resolve().parent / "fixtures"

# Require a relevant diagnostic, so a broken policy cannot mask a DBC validation bug.
REJECTIONS = {
    "bad_timeout": "bounded integer milliseconds",
    "duplicate_policy_key": "duplicate policy key",
    "invalid_identifier": "Invalid syntax",
    "missing_policy_pdu": "missing RX timeout",
    "negative_scale": "positive finite scale",
    "outside_dlc": "does not fit",
    "over_limit_100_messages": r"expected 1\.\.32 messages",
    "over_limit_129_signals": "at most 128 signals",
    "over_limit_33_messages": r"expected 1\.\.32 messages",
    "overlap_signals": "overlapping",
    "syntax_broken_signal": "Invalid syntax",
    "syntax_truncated_message": "Invalid syntax",
    "unsupported_dlc9": "Classical CAN DLC",
    "unsupported_float": "only integer signals",
    "unsupported_multiplex": "multiplexing is unsupported",
    "zero_length_signal": "length 0 is not greater than 0",
    "unsupported_width33": "only integer signals",
    "over_limit_17_signals": r"1\.\.16 signals required",
    "empty_bounds": "empty bounds",
    "unknown_node": "policy node not declared",
    "wrong_receiver": "RX signals must map",
    "duplicate_route": "duplicate normalized message name or CAN route",
    "normalized_signal_collision": "duplicate normalized signal name",
}


class FixtureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location("fixture_codegen", ROOT / "tools/dbc_codegen.py")
        cls.codegen = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.codegen)

    def test_fixture_inventory(self):
        self.assertTrue(list((FIXTURES / "valid").glob("*.dbc")), "no valid fixtures")
        self.assertEqual({p.stem for p in (FIXTURES / "invalid").glob("*.dbc")}, set(REJECTIONS))
        for folder in ("valid", "invalid"):
            self.assertEqual({p.stem for p in (FIXTURES / folder).glob("*.dbc")},
                             {p.stem for p in (FIXTURES / folder).glob("*.yaml")})

    def test_valid_round_trips(self):
        for dbc in sorted((FIXTURES / "valid").glob("*.dbc")):
            with self.subTest(fixture=dbc.name):
                files = self.codegen.generate(dbc, dbc.with_suffix(".yaml"))
                self.assertEqual(set(files), {"can_cfg.c", "can_cfg.h", "can_signals.c", "can_signals.h"})
                counts = check_codec(self, ROOT, self.codegen, dbc, dbc.with_suffix(".yaml"))
                if dbc.stem == "limit_32_messages_128_signals":
                    self.assertEqual(counts, (32, 128))

    def test_dbc_only_round_trips(self):
        fixtures = sorted((FIXTURES / "dbc_only").glob("*.dbc"))
        self.assertTrue(fixtures, "no DBC-only fixtures")
        for dbc in fixtures:
            with self.subTest(fixture=dbc.name):
                check_codec(self, ROOT, self.codegen, dbc, node="ECU")

    def test_invalid_rejected(self):
        for name, diagnostic in REJECTIONS.items():
            with self.subTest(fixture=name):
                dbc = FIXTURES / "invalid" / f"{name}.dbc"
                with self.assertRaisesRegex((ValueError, cantools.database.errors.Error, yaml.YAMLError), diagnostic):
                    self.codegen.generate(dbc, dbc.with_suffix(".yaml"))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, add_help=False)
    parser.add_argument("--repo", type=Path, default=ROOT)
    args, remaining = parser.parse_known_args()
    ROOT = args.repo.resolve()
    if not (ROOT / "tools/dbc_codegen.py").is_file():
        parser.error(f"cannot find generator under {ROOT}; pass --repo")
    unittest.main(argv=[sys.argv[0], *remaining])
