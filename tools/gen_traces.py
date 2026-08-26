"""Generates 40 clean sessions and 30 seeded-violation sessions (10 each
of timing-overrun, timing-underrun and interlock-skip), writes each as a
trace CSV under data/sessions/, and writes data/manifest.json recording,
for every faulty session, the violation the diff harness expects to find.

Every dwell in a clean session is drawn with a margin inside its phase's
timing envelope (never within 500ms of either edge), so a clean session
cannot accidentally straddle a boundary and become a false positive by
chance rather than by a real bug.
"""
from __future__ import annotations

import csv
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


def gen_clean_events(rng: random.Random, num_cycles: int = 3) -> list[tuple[int, str]]:
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


def apply_timing_fault(events, rng, direction: str):
    # direction: "over" or "under". DISPATCH (lo=0) is excluded from
    # "under" since there is no meaningful dwell below 0.
    candidates = [
        i for i in range(len(events) - 1)
        if not (direction == "under" and ENVELOPES_MS[events[i][1]][0] == 0)
    ]
    idx = rng.choice(candidates)
    phase = events[idx][1]
    lo, hi = ENVELOPES_MS[phase]
    if direction == "over":
        bad_dwell = hi + rng.randint(1000, 5000)
    else:
        bad_dwell = max(0, lo - rng.randint(500, max(500, lo - 1)))
    old_dwell = events[idx + 1][0] - events[idx][0]
    shift = bad_dwell - old_dwell
    new_events = events[: idx + 1] + [(ts + shift, ph) for ts, ph in events[idx + 1 :]]
    expected = {"kind": "TIMING_ENVELOPE", "phase": phase, "sample_index": idx + 1}
    return new_events, expected


def apply_interlock_fault(events, rng):
    dispatch_indices = [i for i, (_, ph) in enumerate(events) if ph == "DISPATCH" and i > 0]
    idx = rng.choice(dispatch_indices)
    pred_idx = idx - 1
    ts, _ = events[pred_idx]
    new_events = list(events)
    new_events[pred_idx] = (ts, "LOAD")  # RESTRAINT_CHECK never happened; LOAD ran straight into DISPATCH
    expected = {"kind": "INTERLOCK", "phase": "DISPATCH", "sample_index": idx}
    return new_events, expected


def write_session_csv(path: Path, events: list[tuple[int, str]]) -> None:
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["timestamp_ms", "phase"])
        for ts, phase in events:
            w.writerow([ts, phase])


def main() -> None:
    out_dir = Path(__file__).resolve().parents[1] / "data" / "sessions"
    out_dir.mkdir(parents=True, exist_ok=True)
    manifest = {"clean_sessions": [], "fault_sessions": []}

    rng = random.Random(20260825)  # fixed seed: this manifest is reproducible

    for i in range(40):
        events = gen_clean_events(rng)
        name = f"clean_{i:02d}.csv"
        write_session_csv(out_dir / name, events)
        manifest["clean_sessions"].append(name)

    fault_index = 0
    for direction, count in [("over", 10), ("under", 10)]:
        for _ in range(count):
            base = gen_clean_events(rng)
            events, expected = apply_timing_fault(base, rng, direction)
            name = f"fault_{fault_index:02d}.csv"
            write_session_csv(out_dir / name, events)
            manifest["fault_sessions"].append({"file": name, "expected": expected})
            fault_index += 1
    for _ in range(10):
        base = gen_clean_events(rng)
        events, expected = apply_interlock_fault(base, rng)
        name = f"fault_{fault_index:02d}.csv"
        write_session_csv(out_dir / name, events)
        manifest["fault_sessions"].append({"file": name, "expected": expected})
        fault_index += 1

    with open(out_dir.parent / "manifest.json", "w") as f:
        json.dump(manifest, f, indent=2)

    print(f"wrote {len(manifest['clean_sessions'])} clean sessions, "
          f"{len(manifest['fault_sessions'])} fault sessions to {out_dir}")


if __name__ == "__main__":
    main()
