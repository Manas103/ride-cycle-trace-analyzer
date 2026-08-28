"""Generates framed serial-bus capture sessions: the same controller-cycle
events tools/gen_traces.py produces, but encoded as the byte-oriented
frame format include/trace_engine.hpp and tools/reference_analyzer.py
both decode (SYNC, LEN, SEQ, PAYLOAD, CHECKSUM; see the wire-format
comment in trace_engine.hpp).

This is a sibling generator, not an extension of gen_traces.py, because
the two produce genuinely different artifacts (text CSV rows vs. binary
frames) even though the underlying controller-cycle events they encode
are built the same legal-dwell way.

Sizing: 40 clean framed sessions and 30 seeded-fault framed sessions (10
each of the three framing-violation classes: bad checksum, skipped
sequence number, lost sync/frame alignment), the same 40/30 split
gen_traces.py uses for the CSV path. Framing is a first-class second
capture format with its own fault taxonomy, not a subset of the existing
30/40 budget, so this grows the total session count rather than
redistributing it; see the README Measured results table for the
combined totals across both formats.
"""
from __future__ import annotations

import json
import random
from pathlib import Path

PHASES_ORDER = ["LOAD", "RESTRAINT_CHECK", "DISPATCH", "RUN", "UNLOAD"]
ENVELOPES_MS = {
    "LOAD": (15000, 40000),
    "RESTRAINT_CHECK": (3000, 10000),
    "DISPATCH": (0, 2000),
    "RUN": (20000, 90000),
    "UNLOAD": (10000, 30000),
}
MARGIN_MS = 500

SERIAL_SYNC_BYTE = 0xAA
SERIAL_FRAME_PAYLOAD_LEN = 5
PHASE_CODE_TABLE = ["LOAD", "RESTRAINT_CHECK", "DISPATCH", "RUN", "UNLOAD"]
PHASE_TO_CODE = {p: i for i, p in enumerate(PHASE_CODE_TABLE)}


def gen_clean_events(rng: random.Random, num_cycles: int = 3) -> list[tuple[int, str]]:
    # Same legal-dwell construction as tools/gen_traces.py's
    # gen_clean_events: every dwell is drawn with a margin inside its
    # phase's timing envelope so a clean capture cannot straddle a
    # boundary by chance.
    events = []
    t = 0
    for _ in range(num_cycles):
        for phase in PHASES_ORDER:
            events.append((t, phase))
            lo, hi = ENVELOPES_MS[phase]
            lo_m = lo + MARGIN_MS
            hi_m = hi - MARGIN_MS
            dwell = rng.randint(lo_m, hi_m) if lo_m < hi_m else lo
            t += dwell
    return events


def compute_frame_checksum(length: int, seq: int, payload: bytes) -> int:
    return (length + seq + sum(payload)) & 0xFF


def encode_frame(seq: int, timestamp_ms: int, phase: str) -> bytes:
    payload = timestamp_ms.to_bytes(4, byteorder="little", signed=False) + bytes([PHASE_TO_CODE[phase]])
    length = len(payload)
    checksum = compute_frame_checksum(length, seq, payload)
    return bytes([SERIAL_SYNC_BYTE, length, seq & 0xFF]) + payload + bytes([checksum])


def build_clean_capture(events: list[tuple[int, str]], start_seq: int) -> bytes:
    out = bytearray()
    for i, (ts, phase) in enumerate(events):
        seq = (start_seq + i) & 0xFF
        out += encode_frame(seq, ts, phase)
    return bytes(out)


def apply_checksum_fault(events, rng, start_seq):
    frames = [bytearray(encode_frame((start_seq + i) & 0xFF, ts, phase))
              for i, (ts, phase) in enumerate(events)]
    idx = rng.randrange(len(frames))
    checksum_pos = len(frames[idx]) - 1
    frames[idx][checksum_pos] ^= 0xFF  # flips every bit; cannot equal the original value
    expected = {"kind": "FRAMING_CHECKSUM", "phase": "FRAME", "sample_index": idx}
    capture = bytearray()
    for f in frames:
        capture += f
    return bytes(capture), expected


def apply_sequence_fault(events, rng, start_seq):
    # Skip the sequence counter forward by a few numbers starting at a
    # random frame, and keep every frame from there on shifted by the
    # same amount (checksums recomputed to match), so exactly one
    # FRAMING_SEQUENCE violation is produced, at the frame where the
    # jump happens, rather than a cascade.
    idx = rng.randrange(1, len(events))  # frame 0 always sets the baseline, can't violate on it
    skip = rng.randint(2, 5)
    frames = []
    for i, (ts, phase) in enumerate(events):
        seq = (start_seq + i) & 0xFF
        if i >= idx:
            seq = (seq + skip) & 0xFF
        frames.append(encode_frame(seq, ts, phase))
    expected = {"kind": "FRAMING_SEQUENCE", "phase": "FRAME", "sample_index": idx}
    capture = bytearray()
    for f in frames:
        capture += f
    return bytes(capture), expected


def apply_sync_fault(events, rng, start_seq):
    # Corrupt one frame's sync byte so the decoder loses frame alignment
    # and has to resynchronize on the next 0xAA. Pick a frame whose own
    # remaining bytes (len, seq, payload, checksum) contain no incidental
    # 0xAA, so the decoder resyncs exactly at the start of the next true
    # frame and the seeded violation lands at a single, predictable frame
    # index rather than an unpredictable one.
    frames = [bytearray(encode_frame((start_seq + i) & 0xFF, ts, phase))
              for i, (ts, phase) in enumerate(events)]
    candidates = list(range(len(frames) - 1))  # exclude the last frame: need a following frame to resync onto
    rng.shuffle(candidates)
    for idx in candidates:
        remainder = frames[idx][1:]  # everything but the sync byte itself
        if SERIAL_SYNC_BYTE not in remainder:
            frames[idx][0] = 0x00  # was SERIAL_SYNC_BYTE
            expected = {"kind": "FRAMING_SYNC", "phase": "FRAME", "sample_index": idx}
            capture = bytearray()
            for f in frames:
                capture += f
            return bytes(capture), expected
    raise RuntimeError("no candidate frame free of incidental 0xAA; widen the search or reseed")


def main() -> None:
    out_dir = Path(__file__).resolve().parents[1] / "data" / "frames"
    out_dir.mkdir(parents=True, exist_ok=True)
    manifest = {"clean_sessions": [], "fault_sessions": []}

    rng = random.Random(20260827)  # fixed seed: this manifest is reproducible

    for i in range(40):
        events = gen_clean_events(rng)
        start_seq = rng.randrange(256)
        capture = build_clean_capture(events, start_seq)
        name = f"clean_{i:02d}.bin"
        (out_dir / name).write_bytes(capture)
        manifest["clean_sessions"].append(name)

    fault_index = 0
    fault_builders = [
        ("checksum", apply_checksum_fault),
        ("sequence", apply_sequence_fault),
        ("sync", apply_sync_fault),
    ]
    for label, builder in fault_builders:
        for _ in range(10):
            events = gen_clean_events(rng)
            start_seq = rng.randrange(256)
            capture, expected = builder(events, rng, start_seq)
            name = f"fault_{fault_index:02d}_{label}.bin"
            (out_dir / name).write_bytes(capture)
            manifest["fault_sessions"].append({"file": name, "expected": expected})
            fault_index += 1

    with open(out_dir.parent / "frames_manifest.json", "w") as f:
        json.dump(manifest, f, indent=2)

    print(f"wrote {len(manifest['clean_sessions'])} clean framed sessions, "
          f"{len(manifest['fault_sessions'])} fault framed sessions to {out_dir}")


if __name__ == "__main__":
    main()
