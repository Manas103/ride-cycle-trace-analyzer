"""Runs both engines (the C++ CLI and the independent Python reference)
over every generated session, and reports three numbers: how many of the
30 seeded violations each engine caught, how many false positives each
produced across the 40 clean sessions, and how many sessions the two
engines disagreed on at all (kind, phase and sample index, for every
violation, not just the seeded one). The third number is the actual
"diffed against an independent reference" claim; the first two are
this project's version of "caught the fault, didn't cry wolf".
"""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from reference_analyzer import parse_trace_csv, detect_violations  # noqa: E402


def violation_set(violations) -> set[tuple[str, str, int, int]]:
    return {(v.kind, v.phase, v.sample_index, v.value_ms) for v in violations}


def run_cpp_cli(cli_path: Path, trace_path: Path, out_csv: Path) -> set[tuple[str, str, int, int]]:
    subprocess.run([str(cli_path), str(trace_path), str(out_csv)], check=True,
                   capture_output=True, text=True)
    rows = out_csv.read_text().splitlines()[1:]  # skip header
    result = set()
    for row in rows:
        if not row.strip():
            continue
        parts = row.split(",", 4)
        kind, phase, sample_index, value_ms = parts[0], parts[1], int(parts[2]), int(parts[3])
        result.add((kind, phase, sample_index, value_ms))
    return result


def run_python(trace_path: Path) -> set[tuple[str, str, int, int]]:
    events = parse_trace_csv(str(trace_path))
    violations = detect_violations(events)
    return violation_set(violations)


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print("usage: diff_harness.py <path-to-trace_parser_cli>", file=sys.stderr)
        return 2
    cli_path = Path(argv[1])
    root = Path(__file__).resolve().parents[1]
    sessions_dir = root / "data" / "sessions"
    manifest = json.loads((root / "data" / "manifest.json").read_text())

    scratch = root / "data" / "_diff_scratch.csv"

    false_positives_cpp = 0
    false_positives_py = 0
    for name in manifest["clean_sessions"]:
        trace = sessions_dir / name
        cpp_v = run_cpp_cli(cli_path, trace, scratch)
        py_v = run_python(trace)
        if cpp_v:
            false_positives_cpp += 1
        if py_v:
            false_positives_py += 1
        if cpp_v != py_v:
            print(f"DISAGREEMENT on clean session {name}: cpp={cpp_v} py={py_v}")

    caught_cpp = 0
    caught_py = 0
    disagreements = 0
    for entry in manifest["fault_sessions"]:
        trace = sessions_dir / entry["file"]
        expected = entry["expected"]
        expected_tuple_prefix = (expected["kind"], expected["phase"], expected["sample_index"])

        cpp_v = run_cpp_cli(cli_path, trace, scratch)
        py_v = run_python(trace)

        if any((k, p, s) == expected_tuple_prefix for k, p, s, _ in cpp_v):
            caught_cpp += 1
        if any((k, p, s) == expected_tuple_prefix for k, p, s, _ in py_v):
            caught_py += 1
        if cpp_v != py_v:
            disagreements += 1
            print(f"DISAGREEMENT on fault session {entry['file']}: cpp={cpp_v} py={py_v}")

    scratch.unlink(missing_ok=True)

    total_clean = len(manifest["clean_sessions"])
    total_fault = len(manifest["fault_sessions"])
    print()
    print(f"Seeded violations caught: C++ {caught_cpp}/{total_fault}, Python {caught_py}/{total_fault}")
    print(f"False positives across {total_clean} clean sessions: C++ {false_positives_cpp}, Python {false_positives_py}")
    print(f"Sessions where C++ and Python disagreed (any violation, not just the seeded one): {disagreements} / {total_clean + total_fault}")

    ok = (caught_cpp == total_fault and caught_py == total_fault and
          false_positives_cpp == 0 and false_positives_py == 0 and disagreements == 0)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
