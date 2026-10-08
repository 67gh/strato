#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Summarize Strato NCE JSONL diagnostics, without generating or applying patches.

Usage: python tools/analyze_nce_fallback.py path/to/nce_fallback.jsonl [--json]
Only the Python standard library is required (Python 3.9+).
Schema 2 count rows are cumulative checkpoints, not additional occurrences.
Legacy logs and sessions interrupted before session_end provide lower bounds.
"""

import argparse
import json
import sys
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable, Optional, TextIO


def integer(value: Any) -> Optional[int]:
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        try:
            return int(value, 0)
        except ValueError:
            return None
    return None


def nonnegative(value: Any) -> Optional[int]:
    number = integer(value)
    return number if number is not None and number >= 0 else None


def hex_value(value: Any) -> Optional[str]:
    number = nonnegative(value)
    return f"0x{number:X}" if number is not None else None


@dataclass
class Session:
    source: str
    identity: str
    schema: int = 1
    jit_compiled: Optional[bool] = None
    events: dict = field(default_factory=dict)
    ended: bool = False
    total_faults: Optional[int] = None
    untracked_faults: int = 0
    contended_faults: int = 0
    omitted_lines: int = 0
    write_errno: int = 0
    invalid_records: int = 0
    legacy_records: int = 0


class Analyzer:
    def __init__(self) -> None:
        self.sessions = []
        self.warnings = []
        self.warning_count = 0

    def warn(self, message: str) -> None:
        self.warning_count += 1
        if len(self.warnings) < 100:
            self.warnings.append(message)

    def read(self, stream: TextIO, source: str = "<stream>") -> None:
        current = None
        serial = 0
        for line_number, line in enumerate(stream, 1):
            if not line.strip():
                continue
            location = f"{source}:{line_number}"
            try:
                row = json.loads(line)
                if not isinstance(row, dict):
                    raise ValueError("record is not an object")
            except (ValueError, json.JSONDecodeError) as error:
                self.warn(f"{location}: ligne JSON ignoree ({error})")
                if current:
                    current.invalid_records += 1
                continue

            kind = row.get("type")
            if kind not in {"session", "failure", "count", "overflow", "session_end"}:
                self.warn(f"{location}: type de record inconnu {kind!r}")
                if current:
                    current.invalid_records += 1
                continue
            schema = nonnegative(row.get("schema_version", 1))
            if schema not in (1, 2):
                self.warn(f"{location}: schema non pris en charge {schema!r}")
                if current:
                    current.invalid_records += 1
                continue

            identity = row.get("session_id")
            if identity is not None and not isinstance(identity, str):
                self.warn(f"{location}: session_id invalide")
                if current:
                    current.invalid_records += 1
                continue
            if kind == "session" or current is None or (identity and identity != current.identity):
                serial += 1
                current = Session(source, identity or f"legacy-{serial}", schema=schema)
                self.sessions.append(current)
                if kind != "session":
                    self.warn(f"{location}: en-tete de session absent; metadonnees partielles")
                compiled = row.get("jit_compiled")
                current.jit_compiled = compiled if isinstance(compiled, bool) else None
            if kind == "session":
                continue
            if current.ended:
                current.invalid_records += 1
                current.ended = False
                self.warn(f"{location}: record apres session_end; completude incertaine")

            if kind in {"overflow", "session_end"}:
                for name in ("untracked_faults", "contended_faults", "omitted_lines", "write_errno"):
                    if name in row:
                        value = nonnegative(row[name])
                        if value is None:
                            current.invalid_records += 1
                            self.warn(f"{location}: compteur {name} invalide")
                        else:
                            setattr(current, name, max(getattr(current, name), value))
                if kind == "session_end":
                    current.total_faults = nonnegative(row.get("total_faults"))
                    current.ended = schema == 2 and current.total_faults is not None
                    if not current.ended:
                        current.invalid_records += 1
                        self.warn(f"{location}: fin de session sans total valide")
                continue

            count = nonnegative(row.get("count"))
            event_id = nonnegative(row.get("event_id"))
            legacy = schema == 1 or event_id is None
            if legacy:
                if schema == 2 and event_id is None:
                    current.invalid_records += 1
                    self.warn(f"{location}: event_id absent du schema 2; compte rendu minimal")
                if kind == "count":
                    current.invalid_records += 1
                    self.warn(f"{location}: compteur sans event_id ignore")
                    continue
                current.legacy_records += 1
                event_id = f"sample-{line_number}"
                count = max(count or 1, 1)
            elif count is None or count == 0:
                current.invalid_records += 1
                self.warn(f"{location}: nombre d'occurrences invalide")
                continue

            event = current.events.setdefault(event_id, {"count": 0, "row": {}, "has_sample": False, "legacy": legacy})
            if count < event["count"] and kind == "count":
                current.invalid_records += 1
                self.warn(f"{location}: compteur cumulatif decroissant; maximum conserve")
            event["count"] = max(event["count"], count)
            # Counts can survive a truncated/omitted detailed sample. Preserve sample
            # metadata when later, smaller count-only checkpoints are encountered.
            event["row"].update({key: value for key, value in row.items() if key not in {"type", "count"}})
            event["has_sample"] |= kind == "failure"

    def report(self) -> dict:
        groups = {}
        session_reports = []
        all_exact = bool(self.sessions) and not self.warning_count
        reported_final_total = 0
        sessions_with_final_total = 0
        for session in self.sessions:
            known = sum(event["count"] for event in session.events.values())
            distributed_missing = session.untracked_faults + session.contended_faults
            accounting_matches = session.total_faults == known + distributed_missing
            exact = (session.schema == 2 and session.ended and accounting_matches
                     and not session.invalid_records and not session.legacy_records
                     and not distributed_missing and not session.write_errno)
            all_exact &= exact
            if session.total_faults is not None:
                reported_final_total += session.total_faults
                sessions_with_final_total += 1
            session_reports.append({
                "source": session.source, "session_id": session.identity,
                "schema_version": session.schema, "jit_compiled": session.jit_compiled,
                "has_session_end": session.ended, "observed_occurrences_at_least": known,
                "reported_final_total": session.total_faults, "counts_exact": exact,
                "untracked_faults": session.untracked_faults,
                "contended_faults": session.contended_faults,
                "omitted_lines": session.omitted_lines,
                "invalid_records": session.invalid_records,
                "legacy_records": session.legacy_records,
                "accounting_matches": accounting_matches if session.ended else None,
            })
            for event in session.events.values():
                row = event["row"]
                module = row.get("module")
                offset = integer(row.get("module_offset"))
                module = module if isinstance(module, str) and module else None
                pc = hex_value(row.get("pc"))
                insn = hex_value(row.get("insn"))
                outcome = row.get("outcome", "unknown")
                outcome = outcome if isinstance(outcome, str) else "unknown"
                signal = integer(row.get("signal_number"))
                if signal is None:
                    signal = row.get("signal") if isinstance(row.get("signal"), str) else None
                # Only module-relative locations may aggregate across ASLR sessions.
                # An unidentified absolute PC is scoped to its source/session.
                stable_location = (module, offset) if module and offset is not None else (session.source, session.identity, pc)
                key = (stable_location, insn, outcome, signal, integer(row.get("si_code")))
                if key not in groups:
                    instruction_class = row.get("class", "unavailable")
                    instruction_class = instruction_class if isinstance(instruction_class, str) else "unavailable"
                    groups[key] = {
                        "module": module, "module_offset": offset,
                        "example_pc": pc, "insn": insn, "outcome": outcome,
                        "signal": signal, "si_code": integer(row.get("si_code")),
                        "class": instruction_class, "occurrences_at_least": 0,
                        "legacy_pc_unreliable": False, "detailed_sample_missing": False,
                        "native_strategy": native_strategy(instruction_class, outcome, insn),
                    }
                group = groups[key]
                group["occurrences_at_least"] += event["count"]
                group["legacy_pc_unreliable"] |= event["legacy"] and outcome == "recovered_by_jit"
                group["detailed_sample_missing"] |= not event["has_sample"]
        results = sorted(groups.values(), key=lambda item: (-item["occurrences_at_least"], str(item["module"]), str(item["example_pc"])))
        for group in results:
            group["counts_exact"] = all_exact
            if group["legacy_pc_unreliable"]:
                group["native_strategy"] = "Recapturer avec le schema 2 : un succes de l'ancien logger peut designer le PC apres execution. Aucun patch ne doit etre deduit de ce PC."
        outcomes = Counter()
        for group in results:
            outcomes[group["outcome"]] += group["occurrences_at_least"]
        return {
            "analysis_schema": 1, "counts_exact": all_exact,
            "observed_occurrences_at_least": sum(outcomes.values()),
            "reported_final_total": reported_final_total if sessions_with_final_total else None,
            "sessions_with_final_total": sessions_with_final_total,
            "outcomes": dict(outcomes), "sessions": session_reports,
            "groups": results, "warning_count": self.warning_count, "warnings": self.warnings,
            "limitations": [
                "Sans session_end et reconciliation des compteurs, les frequences sont des minima.",
                "Un succes JIT ne prouve ni l'equivalence NCE/JIT ni la cause d'un gel.",
                "L'ancien schema attribue parfois les recuperations au PC apres execution.",
                "Les strategies sont des pistes de validation manuelle; aucun code natif n'est genere.",
            ],
        }


def native_strategy(instruction_class: str, outcome: str, insn: Optional[str]) -> str:
    if insn is None:
        return "Retablir la lecture sure de l'opcode et collecter le contexte initial avant toute proposition native."
    if outcome != "recovered_by_jit":
        return "Conserver le constat d'echec; verifier signal, mapping et semantique invitee. Aucun remplacement natif n'est etabli."
    if "system register" in instruction_class:
        return "Verifier le contrat du registre invite (TLS, frequence, compteur, permissions), puis comparer des etats controles avant un patch NCE."
    if "load/store" in instruction_class or "atomic" in instruction_class:
        return "Verifier alignement, permissions, ordre memoire et reservations exclusives; ne jamais rejouer deux fois des ecritures sur la memoire reelle."
    if "SIMD" in instruction_class or "FP" in instruction_class:
        return "Comparer registres vectoriels, FPCR/FPSR et cas limites dans un banc isole avant une implementation native."
    return "Confirmer le decodage et la semantique invitee, construire un cas reproductible puis comparer PC, registres et effets memoire isoles."


def text_report(report: dict, top: int = 20) -> str:
    count_label = "exactes" if report["counts_exact"] else "minimales, completude non etablie"
    lines = [f"NCE : {report['observed_occurrences_at_least']} occurrences {count_label} ; {len(report['sessions'])} session(s)."]
    if report["reported_final_total"] is not None:
        lines.append(f"Total final annonce par {report['sessions_with_final_total']} session(s) : {report['reported_final_total']}.")
    for group in report["groups"][:top]:
        location = group["example_pc"] or "PC inconnu"
        if group["module"] and group["module_offset"] is not None:
            offset = group["module_offset"]
            location = f"{group['module']}{'+' if offset >= 0 else '-'}0x{abs(offset):X}"
        lines.append(f"  {group['occurrences_at_least']:>8}  {location}  {group['insn'] or 'opcode inconnu'}  {group['outcome']}")
        lines.append(f"            {group['native_strategy']}")
    lines.extend(f"Attention : {warning}" for warning in report["warnings"][:10])
    if report["warning_count"] > 10:
        lines.append(f"... {report['warning_count'] - 10} autre(s) avertissement(s), consulter --json.")
    lines.extend(report["limitations"])
    return "\n".join(lines)


def main(argv: Optional[Iterable[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("files", nargs="+", type=Path, help="Un ou plusieurs fichiers JSONL NCE")
    parser.add_argument("--json", action="store_true", help="Rapport JSON complet")
    parser.add_argument("--top", type=int, default=20, help="Nombre de groupes affiches en texte (defaut : 20)")
    args = parser.parse_args(argv)
    if args.top < 1:
        parser.error("--top doit etre positif")
    analyzer = Analyzer()
    seen_paths = set()
    for path in args.files:
        resolved = path.resolve()
        if resolved in seen_paths:
            continue
        seen_paths.add(resolved)
        try:
            with path.open("r", encoding="utf-8-sig") as stream:
                analyzer.read(stream, str(path))
        except (OSError, UnicodeError) as error:
            print(f"Lecture impossible : {path}: {error}", file=sys.stderr)
            return 2
    report = analyzer.report()
    print(json.dumps(report, ensure_ascii=False, indent=2) if args.json else text_report(report, args.top))
    return 0


if __name__ == "__main__":
    sys.exit(main())
