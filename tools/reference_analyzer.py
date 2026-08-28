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


# ---------------------------------------------------------------------
# Framed serial-bus capture support
#
# Independent second implementation of the wire format documented in
# include/trace_engine.hpp: a SYNC byte (0xAA), a LEN byte, a SEQ byte, a
# LEN-byte payload (a little-endian uint32 timestamp followed by a
# 1-byte phase code), then an additive mod-256 checksum over
# LEN+SEQ+payload. This module was written from that byte-layout spec,
# not translated from the C++ loop; the control flow below (slicing a
# bytes object and re-scanning with bytes.find for resync) is idiomatic
# Python rather than a line-for-line port of the C++ pointer-walking, the
# same independence discipline as detect_violations above.

SERIAL_SYNC_BYTE = 0xAA
SERIAL_FRAME_PAYLOAD_LEN = 5  # 4-byte timestamp + 1-byte phase code
PHASE_CODE_TABLE = ["LOAD", "RESTRAINT_CHECK", "DISPATCH", "RUN", "UNLOAD"]


def compute_frame_checksum(length: int, seq: int, payload: bytes) -> int:
    return (length + seq + sum(payload)) & 0xFF


def parse_serial_frames(path: str) -> tuple[list[Event], list[Violation], int]:
    with open(path, "rb") as f:
        buf = f.read()

    n = len(buf)
    pos = 0
    frame_index = 0
    frame_count = 0
    expected_seq = -1
    events: list[Event] = []
    violations: list[Violation] = []

    while pos < n:
        if buf[pos] != SERIAL_SYNC_BYTE:
            start = pos
            next_sync = buf.find(bytes([SERIAL_SYNC_BYTE]), pos)
            if next_sync == -1:
                violations.append(Violation(
                    "FRAMING_SYNC", "FRAME", frame_index, -1,
                    f"lost sync at byte {start}, no further sync byte found, "
                    "capture ends misaligned",
                ))
                pos = n
            else:
                violations.append(Violation(
                    "FRAMING_SYNC", "FRAME", frame_index, -1,
                    f"lost sync at byte {start}, resynced at byte {next_sync}",
                ))
                pos = next_sync
            frame_index += 1
            continue

        if pos + 3 > n:
            violations.append(Violation(
                "FRAMING_SYNC", "FRAME", frame_index, -1,
                f"truncated frame header at byte {pos}",
            ))
            frame_count += 1
            break

        length = buf[pos + 1]
        seq = buf[pos + 2]
        frame_total = length + 4  # sync + len + seq + payload + checksum
        if pos + frame_total > n:
            violations.append(Violation(
                "FRAMING_SYNC", "FRAME", frame_index, -1,
                f"truncated frame payload/checksum at byte {pos}",
            ))
            frame_count += 1
            break

        payload = buf[pos + 3: pos + 3 + length]
        checksum_byte = buf[pos + 3 + length]
        computed = compute_frame_checksum(length, seq, payload)
        if computed != checksum_byte:
            violations.append(Violation(
                "FRAMING_CHECKSUM", "FRAME", frame_index, -1,
                f"frame {frame_index} checksum mismatch: computed {computed}, got {checksum_byte}",
            ))

        if expected_seq == -1:
            expected_seq = seq
        elif seq != (expected_seq & 0xFF):
            violations.append(Violation(
                "FRAMING_SEQUENCE", "FRAME", frame_index, -1,
                f"frame {frame_index} expected seq {expected_seq & 0xFF}, got {seq}",
            ))
        expected_seq = seq + 1

        if len(payload) >= SERIAL_FRAME_PAYLOAD_LEN:
            ts = int.from_bytes(payload[0:4], byteorder="little", signed=False)
            phase_code = payload[4]
            phase = PHASE_CODE_TABLE[phase_code] if phase_code < len(PHASE_CODE_TABLE) else "UNKNOWN"
            events.append(Event(ts, phase, frame_index))

        pos += frame_total
        frame_index += 1
        frame_count += 1

    return events, violations, frame_count


def detect_framed_violations(path: str) -> list[Violation]:
    events, framing_violations, _frame_count = parse_serial_frames(path)
    return framing_violations + detect_violations(events)
