"""Runs both engines (the C++ CLI and the independent Python reference)
over every generated session of both capture formats (the plain CSV
trace and the framed serial-bus capture), and reports, per format and
combined: how many seeded violations each engine caught, how many false
positives each produced across the clean sessions, and how many sessions
the two engines disagreed on at all (kind, phase and sample index, for
every violation, not just the seeded one). The disagreement count is the
actual "diffed against an independent reference" claim; the other two are
this project's version of "caught the fault, didn't cry wolf".
"""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from reference_analyzer import (  # noqa: E402
    parse_trace_csv,
    detect_violations,
    detect_framed_violations,
)


def violation_set(violations) -> set[tuple[str, str, int, int]]:
    return {(v.kind, v.phase, v.sample_index, v.value_ms) for v in violations}


def run_cpp_cli(cli_path: Path, cli_args: list[str], out_csv: Path) -> set[tuple[str, str, int, int]]:
    subprocess.run([str(cli_path), *cli_args, str(out_csv)], check=True,
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


def run_python_csv(trace_path: Path) -> set[tuple[str, str, int, int]]:
    events = parse_trace_csv(str(trace_path))
    violations = detect_violations(events)
    return violation_set(violations)


def run_python_framed(capture_path: Path) -> set[tuple[str, str, int, int]]:
    return violation_set(detect_framed_violations(str(capture_path)))


class FormatResult:
    def __init__(self, label: str, total_clean: int, total_fault: int):
        self.label = label
        self.total_clean = total_clean
        self.total_fault = total_fault
        self.false_positives_cpp = 0
        self.false_positives_py = 0
        self.caught_cpp = 0
        self.caught_py = 0
        self.disagreements = 0


def run_format(result: FormatResult, manifest: dict, sessions_dir: Path, scratch: Path,
                cli_path: Path, cli_prefix: list[str], run_python) -> None:
    for name in manifest["clean_sessions"]:
        session = sessions_dir / name
        cpp_v = run_cpp_cli(cli_path, [*cli_prefix, str(session)], scratch)
        py_v = run_python(session)
        if cpp_v:
            result.false_positives_cpp += 1
        if py_v:
            result.false_positives_py += 1
        if cpp_v != py_v:
            print(f"DISAGREEMENT on {result.label} clean session {name}: cpp={cpp_v} py={py_v}")

    for entry in manifest["fault_sessions"]:
        session = sessions_dir / entry["file"]
        expected = entry["expected"]
        expected_tuple_prefix = (expected["kind"], expected["phase"], expected["sample_index"])

        cpp_v = run_cpp_cli(cli_path, [*cli_prefix, str(session)], scratch)
        py_v = run_python(session)

        if any((k, p, s) == expected_tuple_prefix for k, p, s, _ in cpp_v):
            result.caught_cpp += 1
        if any((k, p, s) == expected_tuple_prefix for k, p, s, _ in py_v):
            result.caught_py += 1
        if cpp_v != py_v:
            result.disagreements += 1
            print(f"DISAGREEMENT on {result.label} fault session {entry['file']}: cpp={cpp_v} py={py_v}")


def print_format_result(result: FormatResult) -> None:
    print(f"[{result.label}] Seeded violations caught: C++ {result.caught_cpp}/{result.total_fault}, "
          f"Python {result.caught_py}/{result.total_fault}")
    print(f"[{result.label}] False positives across {result.total_clean} clean sessions: "
          f"C++ {result.false_positives_cpp}, Python {result.false_positives_py}")
    print(f"[{result.label}] Sessions where C++ and Python disagreed: "
          f"{result.disagreements} / {result.total_clean + result.total_fault}")


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print("usage: diff_harness.py <path-to-trace_parser_cli>", file=sys.stderr)
        return 2
    cli_path = Path(argv[1])
    root = Path(__file__).resolve().parents[1]
    scratch = root / "data" / "_diff_scratch.csv"

    csv_sessions_dir = root / "data" / "sessions"
    csv_manifest = json.loads((root / "data" / "manifest.json").read_text())
    csv_result = FormatResult("csv", len(csv_manifest["clean_sessions"]), len(csv_manifest["fault_sessions"]))
    run_format(csv_result, csv_manifest, csv_sessions_dir, scratch, cli_path, [], run_python_csv)

    frames_sessions_dir = root / "data" / "frames"
    frames_manifest = json.loads((root / "data" / "frames_manifest.json").read_text())
    frames_result = FormatResult("framed", len(frames_manifest["clean_sessions"]),
                                  len(frames_manifest["fault_sessions"]))
    run_format(frames_result, frames_manifest, frames_sessions_dir, scratch, cli_path,
               ["--framed"], run_python_framed)

    scratch.unlink(missing_ok=True)

    print()
    print_format_result(csv_result)
    print_format_result(frames_result)

    total_clean = csv_result.total_clean + frames_result.total_clean
    total_fault = csv_result.total_fault + frames_result.total_fault
    total_sessions = total_clean + total_fault
    caught_cpp = csv_result.caught_cpp + frames_result.caught_cpp
    caught_py = csv_result.caught_py + frames_result.caught_py
    fp_cpp = csv_result.false_positives_cpp + frames_result.false_positives_cpp
    fp_py = csv_result.false_positives_py + frames_result.false_positives_py
    disagreements = csv_result.disagreements + frames_result.disagreements

    print()
    print(f"[combined] Seeded violations caught: C++ {caught_cpp}/{total_fault}, Python {caught_py}/{total_fault}")
    print(f"[combined] False positives across {total_clean} clean sessions: C++ {fp_cpp}, Python {fp_py}")
    print(f"[combined] Sessions where C++ and Python disagreed (any violation, not just the seeded one): "
          f"{disagreements} / {total_sessions}")

    ok = (caught_cpp == total_fault and caught_py == total_fault and
          fp_cpp == 0 and fp_py == 0 and disagreements == 0)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
