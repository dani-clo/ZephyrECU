"""DBC attributes, optional/partial YAML, node selection and CLI compatibility."""
from contextlib import contextmanager
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("optional_codegen", ROOT / "tools/dbc_codegen.py")
codegen = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(codegen)
EXAMPLES = ROOT / "tests/codegen/fixtures/dbc_only"
ATTRIBUTES = (EXAMPLES / "attributes_override.dbc").read_text()
MINIMAL = (EXAMPLES / "minimal_dbc_only.dbc").read_text()


@contextmanager
def inputs(text=ATTRIBUTES, policy=None):
    with tempfile.TemporaryDirectory() as directory:
        dbc = Path(directory) / "input.dbc"
        dbc.write_text(text)
        settings = None
        if policy is not None:
            settings = Path(directory) / "policy.yml"
            settings.write_text(yaml.safe_dump(policy))
        yield dbc, settings


def configuration(dbc, policy=None, node="ECU"):
    pdus, signals = codegen.model(dbc, policy, node=node)
    return ({p["message"].name: (p["mode"], p["period"], p["delay"], p["timeout"]) for p in pdus},
            {s["signal"].name: s["trigger"] for s in signals})


class OptionalPolicyTests(unittest.TestCase):
    def test_attributes(self):
        pdus, triggers = configuration(EXAMPLES / "attributes_override.dbc")
        self.assertEqual(pdus, {
            "ECU_Periodic": ("PERIODIC", 50, 0, 0),
            "ECU_Event": ("EVENT", 0, 20, 0),
            "ECU_Mixed": ("MIXED", 100, 10, 0),
            "Tester_Input": ("RX", 0, 0, 250),
        })
        self.assertEqual(triggers, {"Counter": "PENDING", "Health": "PENDING",
                                   "EventFlag": "TRIGGERED_ON_CHANGE",
                                   "Temperature": "TRIGGERED_ON_CHANGE", "Switch": "PENDING"})

    def test_partial_overrides(self):
        original_pdus, original_triggers = configuration(EXAMPLES / "attributes_override.dbc")
        override = {"pdus": {
            "ECU_Periodic": {"mode": "MIXED", "period_ms": 75, "min_delay_ms": 3,
                             "signals": {"Counter": "TRIGGERED_ON_CHANGE"}},
            "ECU_Event": {"min_delay_ms": 5},
            "Tester_Input": {"timeout_ms": 600},
        }}
        with inputs(policy=override) as paths:
            pdus, triggers = configuration(*paths)
        original_pdus.update(ECU_Periodic=("MIXED", 75, 3, 0), ECU_Event=("EVENT", 0, 5, 0),
                             Tester_Input=("RX", 0, 0, 600))
        original_triggers["Counter"] = "TRIGGERED_ON_CHANGE"
        self.assertEqual(pdus, original_pdus)
        self.assertEqual(triggers, original_triggers)

    def test_empty_overrides_and_policy_node(self):
        expected = codegen.generate(EXAMPLES / "attributes_override.dbc", node="ECU")
        for policy in ({}, {"node": "ECU"}, {"pdus": {}}, {"pdus": {"ECU_Periodic": {}}}):
            with self.subTest(policy=policy), inputs(policy=policy) as paths:
                self.assertEqual(codegen.generate(*paths, node="ECU"), expected)
        with inputs(policy={"node": "ECU"}) as paths:
            self.assertEqual(codegen.generate(*paths), expected)

    def test_safe_defaults(self):
        text = '\n'.join(line for line in MINIMAL.splitlines()
                         if not line.startswith('BA_ ') or 'RxTimeout' in line)
        with inputs(text) as paths:
            pdus, triggers = configuration(*paths)
        self.assertEqual(pdus["ECU_Status"], ("EVENT", 0, 0, 0))
        self.assertEqual(set(triggers.values()), {"PENDING"})
        # A cycle alone is sufficient to select PERIODIC; no implicit cycle is invented.
        text += '\nBA_ "GenMsgCycleTime" BO_ 256 200;\n'
        with inputs(text) as paths:
            self.assertEqual(configuration(*paths)[0]["ECU_Status"], ("PERIODIC", 200, 0, 0))

    def test_attribute_defaults(self):
        text = '\n'.join(line for line in MINIMAL.splitlines() if not line.startswith('BA_ '))
        text += '''
BA_DEF_DEF_ "GenMsgCycleTime" 60;
BA_DEF_DEF_ "GenMsgSendType" "Cyclic";
BA_DEF_DEF_ "GenMsgDelayTime" 2;
BA_DEF_DEF_ "ZephyrECU_RxTimeoutMs" 300;
BA_DEF_DEF_ "ZephyrECU_SignalTrigger" "TRIGGERED_ON_CHANGE";
'''
        with inputs(text) as paths:
            pdus, triggers = configuration(*paths)
        self.assertEqual(pdus["ECU_Status"], ("PERIODIC", 60, 2, 0))
        self.assertEqual(pdus["Tester_Command"], ("RX", 0, 0, 300))
        self.assertEqual(triggers["AliveCounter"], "TRIGGERED_ON_CHANGE")
        self.assertEqual(triggers["Enable"], "PENDING")  # TX attributes do not affect RX.
        with inputs(text + '\nBA_ "GenMsgCycleTime" BO_ 256 70;\n') as paths:
            self.assertEqual(configuration(*paths)[0]["ECU_Status"][1], 70)

    def test_enum_labels_not_fixed_indices(self):
        text = ATTRIBUTES.replace('ENUM "Cyclic","OnWrite","CyclicAndOnWrite"',
                                  'ENUM "OnWrite","CyclicAndOnWrite","Cyclic"')
        text = text.replace('"GenMsgSendType" BO_ 288 0', '"GenMsgSendType" BO_ 288 2')
        text = text.replace('"GenMsgSendType" BO_ 289 1', '"GenMsgSendType" BO_ 289 0')
        text = text.replace('"GenMsgSendType" BO_ 290 2', '"GenMsgSendType" BO_ 290 1')
        with inputs(text) as paths:
            self.assertEqual(configuration(*paths), configuration(EXAMPLES / "attributes_override.dbc"))

    def test_required_rx_timeout(self):
        text = MINIMAL.replace('BA_ "ZephyrECU_RxTimeoutMs" BO_ 512 500;', '')
        with inputs(text) as paths, self.assertRaisesRegex(ValueError, 'missing RX timeout.*ZephyrECU_RxTimeoutMs'):
            codegen.generate(*paths, node="ECU")
        with inputs(text, {"pdus": {"Tester_Command": {"timeout_ms": 200}}}) as paths:
            self.assertEqual(configuration(*paths)[0]["Tester_Command"][3], 200)

    def test_node_selection(self):
        for text, pattern in ((MINIMAL, "ambiguous ECU node"),
                              (MINIMAL.replace('BU_: ECU Tester', 'BU_:'), "missing or ambiguous")):
            with self.subTest(pattern=pattern), inputs(text) as paths:
                with self.assertRaisesRegex(ValueError, pattern):
                    codegen.generate(*paths)
        with inputs(MINIMAL.replace('BU_: ECU Tester', 'BU_: ECU')) as paths:
            self.assertEqual(codegen.generate(*paths), codegen.generate(*paths, node="ECU"))
        with inputs(policy={"node": "Tester"}) as paths:
            with self.assertRaisesRegex(ValueError, "conflicts with policy node"):
                codegen.generate(*paths, node="ECU")
        for node in ("Unknown", ""):
            with self.subTest(node=node), self.assertRaisesRegex(ValueError, "not declared"):
                codegen.generate(EXAMPLES / "attributes_override.dbc", node=node)

    def test_unknown_keys_and_malformed_overrides(self):
        cases = [
            ({"extra": 1}, "unknown keys"),
            ({"pdus": {"Unknown": {}}}, "unknown keys"),
            ({"pdus": {"ECU_Periodic": {"can_id": 100}}}, "unknown keys"),
            ({"pdus": {"ECU_Periodic": {"signals": {"Unknown": "PENDING"}}}}, "unknown keys"),
            ({"pdus": {"Tester_Input": {"mode": "EVENT"}}}, "unknown keys"),
            ({"pdus": {"ECU_Event": {"timeout_ms": 100}}}, "unknown keys"),
            ({"node": None}, "expected a DBC node name"),
            ({"pdus": None}, "expected a mapping"),
            ({"pdus": []}, "expected a mapping"),
            ({"pdus": {"ECU_Periodic": None}}, "expected a mapping"),
            ({"pdus": {"ECU_Periodic": {"signals": None}}}, "expected a mapping"),
            ({"pdus": {"ECU_Periodic": {"signals": {"Counter": None}}}}, "unsupported signal trigger"),
            ({"pdus": {"ECU_Periodic": {"signals": {"Counter": "UNKNOWN"}}}}, "unsupported signal trigger"),
            ({"pdus": {"ECU_Periodic": {"mode": None}}}, "unsupported TX mode"),
            ({"pdus": {"ECU_Periodic": {"mode": "UNKNOWN"}}}, "unsupported TX mode"),
        ]
        for policy, pattern in cases:
            with self.subTest(policy=policy), inputs(policy=policy) as paths:
                with self.assertRaisesRegex(ValueError, pattern):
                    codegen.generate(*paths, node="ECU")
        for key in ("period_ms", "min_delay_ms", "timeout_ms"):
            for value in (True, 1.5, "10", None, -1, 2147483648):
                name = "Tester_Input" if key == "timeout_ms" else "ECU_Periodic"
                with self.subTest(key=key, value=value), inputs(policy={"pdus": {name: {key: value}}}) as paths:
                    with self.assertRaisesRegex(ValueError, "bounded integer milliseconds"):
                        codegen.generate(*paths, node="ECU")

    def test_timing_boundaries(self):
        for period, delay, timeout in ((1, 0, 1), (2147483647, 2147483647, 2147483647)):
            policy = {"pdus": {"ECU_Periodic": {"period_ms": period, "min_delay_ms": delay},
                               "Tester_Input": {"timeout_ms": timeout}}}
            with self.subTest(period=period), inputs(policy=policy) as paths:
                pdus, _ = configuration(*paths)
                self.assertEqual(pdus["ECU_Periodic"], ("PERIODIC", period, delay, 0))
                self.assertEqual(pdus["Tester_Input"][3], timeout)
        with inputs(policy={"pdus": {"Tester_Input": {"timeout_ms": 0}}}) as paths:
            with self.assertRaisesRegex(ValueError, "bounded integer milliseconds"):
                codegen.generate(*paths, node="ECU")

    def test_invalid_attributes_and_combinations(self):
        cases = [
            ('"GenMsgCycleTime" BO_ 288 50', '"GenMsgCycleTime" BO_ 288 -1', "milliseconds"),
            ('"GenMsgDelayTime" BO_ 289 20', '"GenMsgDelayTime" BO_ 289 -1', "milliseconds"),
            ('"ZephyrECU_RxTimeoutMs" BO_ 544 250', '"ZephyrECU_RxTimeoutMs" BO_ 544 0', "milliseconds"),
            ('"GenMsgCycleTime" BO_ 288 50', '"GenMsgCycleTime" BO_ 288 0', "PERIODIC requires positive"),
            ('"GenMsgCycleTime" BO_ 290 100', '"GenMsgCycleTime" BO_ 290 0', "MIXED requires positive"),
            ('"GenMsgCycleTime" BO_ 289 0', '"GenMsgCycleTime" BO_ 289 10', "EVENT requires"),
            ('"Cyclic","OnWrite","CyclicAndOnWrite"', '"Unknown","OnWrite","CyclicAndOnWrite"', "unsupported GenMsgSendType"),
            ('"GenMsgSendType" BO_ 288 0', '"GenMsgSendType" BO_ 288 9', "invalid DBC enum"),
            ('"GenMsgSendType" BO_ 288 0', '"GenMsgSendType" BO_ 288 -1', "invalid DBC enum"),
            ('"ZephyrECU_SignalTrigger" SG_ 288 Counter 0', '"ZephyrECU_SignalTrigger" SG_ 288 Counter 9', "invalid DBC enum"),
            ('ENUM "PENDING","TRIGGERED_ON_CHANGE"', 'ENUM "UNKNOWN","TRIGGERED_ON_CHANGE"', "unsupported signal trigger"),
            ('"GenMsgCycleTime" INT', '"GenMsgCycleTime" FLOAT', "expected BO_ INT"),
        ]
        for old, new, pattern in cases:
            with self.subTest(change=new), inputs(ATTRIBUTES.replace(old, new)) as paths:
                with self.assertRaisesRegex(ValueError, pattern):
                    codegen.generate(*paths, node="ECU")
        for policy, pattern in (({"mode": "EVENT"}, "EVENT requires"),
                                ({"mode": "PERIODIC", "period_ms": 0}, "requires positive")):
            with self.subTest(policy=policy), inputs(policy={"pdus": {"ECU_Periodic": policy}}) as paths:
                with self.assertRaisesRegex(ValueError, pattern):
                    codegen.generate(*paths, node="ECU")

    def test_explicit_overrides_replace_invalid_attribute_values(self):
        text = ATTRIBUTES.replace('ENUM "Cyclic","OnWrite","CyclicAndOnWrite"',
                                  'ENUM "Unknown","OnWrite","CyclicAndOnWrite"')
        text = text.replace('"GenMsgDelayTime" BO_ 288 0', '"GenMsgDelayTime" BO_ 288 -1')
        override = {"pdus": {"ECU_Periodic": {"mode": "PERIODIC", "min_delay_ms": 0}}}
        with inputs(text, override) as paths:
            self.assertEqual(configuration(*paths), configuration(EXAMPLES / "attributes_override.dbc"))

    def test_demo_output_unchanged(self):
        files = codegen.generate(ROOT / "dbc/automotive.dbc", ROOT / "dbc/can_policy.yaml")
        for name, content in files.items():
            self.assertEqual(content.encode(), (ROOT / "components/can_comm/generated" / name).read_bytes())

    def test_cli_dbc_only_and_read_only_check(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "generated"
            command = [sys.executable, str(ROOT / "tools/dbc_codegen.py"),
                       "--dbc", str(EXAMPLES / "minimal_dbc_only.dbc"), "--node", "ECU", "--out", str(output)]
            subprocess.run(command, check=True, capture_output=True)
            expected = codegen.generate(EXAMPLES / "minimal_dbc_only.dbc", node="ECU")
            self.assertEqual({p.name: p.read_text() for p in output.iterdir()}, expected)
            subprocess.run([*command, "--check"], check=True, capture_output=True)
            target = output / "can_cfg.h"
            target.write_text("stale\n")
            before = {p.name: p.read_bytes() for p in output.iterdir()}
            result = subprocess.run([*command, "--check"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertIn("stale generated file", result.stderr)
            self.assertEqual(before, {p.name: p.read_bytes() for p in output.iterdir()})
            result = subprocess.run([*command, "--policy", str(Path(directory) / "missing.yml")],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertIn("DBC generation failed", result.stderr)
            self.assertEqual(before, {p.name: p.read_bytes() for p in output.iterdir()})


if __name__ == "__main__":
    unittest.main()
