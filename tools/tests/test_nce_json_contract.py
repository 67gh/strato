# SPDX-License-Identifier: MPL-2.0
"""Check the actual producer's JSON format literals without an Android compiler.

These tests exercise the portable fmt replacement-field subset shared with
Python str.format. They verify the JSON wire contract, not C++ execution,
instruction emulation, Android signal behavior or memory safety.
"""
import json
import re
import unittest
from pathlib import Path

SOURCE = (Path(__file__).resolve().parents[2] / "app/src/main/cpp/skyline/nce/jit_fallback.cpp").read_text(encoding="utf-8")


def producer_format(after):
    start = SOURCE.index(after)
    match = re.search(r'R"\((.*?)\)"((?:\s*"(?:[^"\\]|\\.)*")*)', SOURCE[start:], re.DOTALL)
    if match is None:
        raise AssertionError(f"No raw format literal after {after}")
    suffix = "".join(json.loads(token) for token in re.findall(r'"(?:[^"\\]|\\.)*"', match[2]))
    return match[1] + suffix


def escaped(value):
    return json.dumps(value, ensure_ascii=False)[1:-1]


class NceJsonContractTests(unittest.TestCase):
    def test_full_failure_keeps_initial_pc_and_result_pc_distinct(self):
        location = producer_format("std::string LocationJson").format(escaped('main "test"\\'), -16, "patch", 32)
        jit = producer_format("job.trace = fmt::format").format(0x2000, '"x0":["0x1","0x2"]', '"R64@0x3000=0x4"', 1, "false", "")
        first = producer_format("auto line{fmt::format").format(
            "session-1", 7, 100, 90, 11, 2, 0x1000, '"0xD53BE040"', 0,
            "MRS", "system register access", 4, 1, 0x1000,
            "instruction_signal", "recovered_by_jit", escaped('detail\n\t"quoted"'))
        second = producer_format("line += fmt::format").format(
            0x1000, 0x3000, 0, ','.join(['"0x1"'] * 31), "true", 0, 0,
            "true", 0x2000, 0x3000, 0, location, jit)
        row = json.loads(first + second)
        self.assertEqual(row["pc"], "0x1000")
        self.assertEqual(row["initial_state"]["pc"], "0x1000")
        self.assertEqual(row["resumed_state"]["pc"], "0x2000")
        self.assertEqual(row["jit"]["next_pc"], "0x2000")
        self.assertEqual(len(row["initial_state"]["x"]), 31)
        self.assertFalse(row["cause_verified"])
        self.assertEqual(row["module_offset"], -16)
        self.assertEqual(row["detail"], 'detail\n\t"quoted"')

    def test_missing_opcode_remains_json_null(self):
        first = producer_format("auto line{fmt::format").format(
            "session-1", 1, 100, 90, 11, -1, 0xFFFFFFFF, "null", 5,
            "unavailable", "unavailable", 11, 1, 0xFFFFFFFF,
            "memory_signal", "not_attempted", "opcode read failed")
        second = producer_format("line += fmt::format").format(
            0xFFFFFFFF, 0, 0, "", "false", 0, 0, "false", 0xFFFFFFFF, 0, 0, "", "")
        self.assertIsNone(json.loads(first + second)["insn"])

    def test_cumulative_count_and_session_end_are_valid_json(self):
        count = producer_format("void WriteCount").format(
            "session-1", 1, 123, 1000, 0x1000, '"0xD53BE040"', 0, 4, 1, "recovered_by_jit", "")
        end = producer_format("void FlushLocked").format("session-1", 2000, 1000, 126, 1, 2, 1, 0, 0)
        self.assertEqual(json.loads(count)["count"], 123)
        self.assertEqual(json.loads(end)["total_faults"], 126)

    def test_session_header_accepts_json_escaped_names(self):
        game = 'game "quote"\n\t\\\x00'
        header = producer_format("void JitFallback::Initialize").format(
            "session-1", escaped(game), 2000, 1000, 42, "true", 0, 1024, 8 * 1024 * 1024)
        self.assertEqual(json.loads(header)["game"], game)
        self.assertEqual(json.loads(header)["schema_version"], 2)


if __name__ == "__main__":
    unittest.main()
