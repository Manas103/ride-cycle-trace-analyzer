"""Independent Python re-implementation of the trace analysis rules in
include/trace_engine.hpp. Deliberately not calling into, importing, or
transliterating the C++ code line-for-line: this module was written from
the same English-language rules (a timing envelope per phase, and
DISPATCH must be immediately preceded by RESTRAINT_CHECK) so that an
agreement between the two is evidence the *rules* are being applied
correctly, not evidence that one implementation copied the other's bugs.
"""
from __future__ import annotations

import csv
from dataclasses import dataclass

TIMING_ENVELOPES_MS = {
    "LOAD": (15000, 40000),
    "RESTRAINT_CHECK": (3000, 10000),
    "DISPATCH": (0, 2000),
    "RUN": (20000, 90000),
    "UNLOAD": (10000, 30000),
}
REQUIRED_PREDECESSOR_OF_DISPATCH = "RESTRAINT_CHECK"


@dataclass(frozen=True)
class Event:
    timestamp_ms: int
    phase: str
    sample_index: int


@dataclass(frozen=True)
class Violation:
    kind: str  # "TIMING_ENVELOPE" or "INTERLOCK"
    phase: str
    sample_index: int
    value_ms: int
    detail: str


def parse_trace_csv(path: str) -> list[Event]:
    events: list[Event] = []
    with open(path, newline="") as f:
        rows = list(csv.reader(f))
    idx = 0
    for i, row in enumerate(rows):
        if not row:
            continue
        if i == 0 and row[0] == "timestamp_ms":
            continue
        ts, phase = row[0], row[1]
        events.append(Event(int(ts), phase, idx))
        idx += 1
    return events


def detect_violations(events: list[Event]) -> list[Violation]:
    violations: list[Violation] = []
    for i in range(len(events) - 1):
        cur, nxt = events[i], events[i + 1]
        dwell = nxt.timestamp_ms - cur.timestamp_ms

        envelope = TIMING_ENVELOPES_MS.get(cur.phase)
        if envelope is not None:
            lo, hi = envelope
            if dwell < lo or dwell > hi:
                violations.append(Violation(
                    "TIMING_ENVELOPE", cur.phase, nxt.sample_index, dwell,
                    f"{cur.phase} dwelled {dwell}ms, outside [{lo}, {hi}]",
                ))

        if nxt.phase == "DISPATCH" and cur.phase != REQUIRED_PREDECESSOR_OF_DISPATCH:
            violations.append(Violation(
                "INTERLOCK", "DISPATCH", nxt.sample_index, -1,
                f"DISPATCH preceded by {cur.phase}, not {REQUIRED_PREDECESSOR_OF_DISPATCH}",
            ))
    return violations


def violations_to_csv_rows(violations: list[Violation]) -> list[str]:
    rows = ["kind,phase,sample_index,value_ms,detail"]
    for v in violations:
        rows.append(f"{v.kind},{v.phase},{v.sample_index},{v.value_ms},{v.detail}")
    return rows
