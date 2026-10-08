# SPDX-License-Identifier: MPL-2.0
import importlib.util
import io
import json
import sys
import unittest
from pathlib import Path

MODULE_PATH = Path(__file__).resolve().parents[1] / "analyze_nce_fallback.py"
SPEC = importlib.util.spec_from_file_location("analyze_nce_fallback", MODULE_PATH)
analyzer_module = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = analyzer_module
SPEC.loader.exec_module(analyzer_module)


def session(name="one"):
    return {"type": "session", "schema_version": 2, "session_id": name, "jit_compiled": True}


def failure(name="one", event_id=1, **fields):
    result = {
        "type": "failure", "schema_version": 2, "session_id": name,
        "event_id": event_id, "count": 1, "pc": "0x1000", "insn": "0xD53BE040",
        "module": "main", "module_offset": 16, "signal_number": 4, "si_code": 1,
        "class": "system register access", "outcome": "recovered_by_jit",
    }
    result.update(fields)
    return result


def count(value, **fields):
    result = failure(**fields)
    result.update(type="count", count=value)
    result.pop("class", None)
    return result


def end(value, name="one", **fields):
    result = {
        "type": "session_end", "schema_version": 2, "session_id": name,
        "total_faults": value, "untracked_faults": 0, "contended_faults": 0,
        "omitted_lines": 0, "write_errno": 0,
    }
    result.update(fields)
    return result


def analyze(*rows):
    analyzer = analyzer_module.Analyzer()
    data = "\n".join(row if isinstance(row, str) else json.dumps(row) for row in rows)
    analyzer.read(io.StringIO(data), "fixture.jsonl")
    return analyzer.report()


class AnalyzeNceFallbackTests(unittest.TestCase):
    def test_cumulative_checkpoints_are_not_added_together(self):
        report = analyze(session(), failure(), count(2), count(4), count(5), end(5))
        self.assertEqual(report["observed_occurrences_at_least"], 5)
        self.assertTrue(report["counts_exact"])
        self.assertEqual(report["groups"][0]["class"], "system register access")

    def test_hard_kill_is_a_lower_bound_even_with_a_checkpoint(self):
        report = analyze(session(), failure(), count(4))
        self.assertEqual(report["observed_occurrences_at_least"], 4)
        self.assertFalse(report["counts_exact"])
        self.assertIsNone(report["reported_final_total"])

    def test_same_event_id_in_separate_sessions_and_aslr(self):
        report = analyze(session(), failure(), count(3), end(3),
                         session("two"), failure(name="two", pc="0x9000"),
                         count(5, name="two", pc="0x9000"), end(5, name="two"))
        self.assertEqual(report["observed_occurrences_at_least"], 8)
        self.assertEqual(len(report["groups"]), 1)
        self.assertTrue(report["counts_exact"])

    def test_unidentified_absolute_pc_does_not_merge_across_sessions(self):
        report = analyze(session(), failure(module=None, module_offset=None), end(1),
                         session("two"), failure(name="two", module=None, module_offset=None), end(1, name="two"))
        self.assertEqual(len(report["groups"]), 2)

    def test_legacy_recovery_warns_about_pc_attribution(self):
        report = analyze({"type": "session", "game": "old"},
                         {"type": "failure", "pc": "0x1004", "insn": "0xD503201F", "outcome": "recovered_by_jit"})
        self.assertEqual(report["observed_occurrences_at_least"], 1)
        self.assertFalse(report["counts_exact"])
        self.assertTrue(report["groups"][0]["legacy_pc_unreliable"])
        self.assertIn("Recapturer", report["groups"][0]["native_strategy"])

    def test_unreadable_opcode_is_not_zero_instruction(self):
        report = analyze(session(), failure(insn=None), failure(event_id=2, insn="0x00000000"), end(2))
        self.assertEqual(len(report["groups"]), 2)
        unreadable = next(group for group in report["groups"] if group["insn"] is None)
        self.assertIn("lecture sure", unreadable["native_strategy"])

    def test_truncated_row_preserves_prior_count_but_not_completeness(self):
        report = analyze(session(), failure(), count(4), '{"type":"count",', end(4))
        self.assertEqual(report["observed_occurrences_at_least"], 4)
        self.assertFalse(report["counts_exact"])
        self.assertEqual(report["warning_count"], 1)

    def test_final_count_can_recover_missing_detailed_sample(self):
        report = analyze(session(), count(9), end(9, omitted_lines=1))
        self.assertEqual(report["observed_occurrences_at_least"], 9)
        self.assertTrue(report["counts_exact"])
        self.assertTrue(report["groups"][0]["detailed_sample_missing"])

    def test_export_tail_without_header_is_not_called_exact(self):
        report = analyze(count(9), end(9))
        self.assertEqual(report["observed_occurrences_at_least"], 9)
        self.assertFalse(report["counts_exact"])
        self.assertTrue(report["groups"][0]["detailed_sample_missing"])
        self.assertEqual(report["warning_count"], 1)

    def test_overflow_and_contention_are_not_attributed_to_known_instruction(self):
        report = analyze(session(), failure(), count(5), end(8, untracked_faults=2, contended_faults=1))
        self.assertEqual(report["observed_occurrences_at_least"], 5)
        self.assertEqual(report["reported_final_total"], 8)
        self.assertFalse(report["groups"][0]["counts_exact"])
        self.assertTrue(report["sessions"][0]["accounting_matches"])

    def test_decreasing_cumulative_count_does_not_erase_observations(self):
        report = analyze(session(), failure(), count(8), count(4), end(8))
        self.assertEqual(report["observed_occurrences_at_least"], 8)
        self.assertFalse(report["counts_exact"])

    def test_final_accounting_mismatch_is_not_called_exact(self):
        report = analyze(session(), failure(), count(4), end(7))
        self.assertFalse(report["counts_exact"])
        self.assertFalse(report["sessions"][0]["accounting_matches"])

    def test_missing_count_or_event_id_is_explicit(self):
        missing_count = failure()
        missing_count.pop("count")
        missing_id = failure()
        missing_id.pop("event_id")
        report = analyze(session(), missing_count, missing_id, end(2))
        self.assertFalse(report["counts_exact"])
        self.assertEqual(report["warning_count"], 2)

    def test_negative_module_offset_and_escaped_names(self):
        report = analyze(session(), failure(module='module "test"\n\\', module_offset=-64), end(1))
        self.assertEqual(report["groups"][0]["module_offset"], -64)
        self.assertIn("-0x40", analyzer_module.text_report(report))
        self.assertEqual(json.loads(json.dumps(report))["groups"][0]["module"], 'module "test"\n\\')

    def test_unknown_schema_and_leading_damage_preclude_exact_total(self):
        report = analyze('{"broken":', {"type": "session", "schema_version": 99}, session(), failure(), end(1))
        self.assertFalse(report["counts_exact"])
        self.assertEqual(report["warning_count"], 2)

    def test_unsupported_jit_does_not_suggest_native_replacement(self):
        report = analyze(session(), failure(outcome="jit_unsupported"), end(1))
        self.assertIn("Aucun remplacement natif", report["groups"][0]["native_strategy"])

    def test_no_records_is_not_an_exact_zero(self):
        report = analyze()
        self.assertFalse(report["counts_exact"])


if __name__ == "__main__":
    unittest.main()
