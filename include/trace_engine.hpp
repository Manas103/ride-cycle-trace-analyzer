#pragma once
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

// A raw controller trace is a sequence of phase-transition events: the
// timestamp (milliseconds since session start) at which the controller
// entered a named phase. This engine reconstructs per-phase dwell times
// and dispatch intervals from that sequence and flags two kinds of
// problem: a phase that stayed active longer or shorter than its allowed
// timing envelope, and a phase transition the controller should never
// make (an interlock violation, e.g. dispatching without a restraint
// check immediately before it).
struct TraceEvent {
    long long timestamp_ms;
    std::string phase;
    int sample_index; // the row this event came from, 0-based
};

struct TimingEnvelope {
    long long min_ms;
    long long max_ms;
};

// The five phases a normal ride cycle passes through, in order, and the
// dwell time each is allowed to take. Shared between the C++ CLI, the DLL
// the WPF app P/Invokes into, and (independently re-typed, not shared) the
// Python reference, so "timing envelope" means the same numbers everywhere
// it is checked.
inline const std::map<std::string, TimingEnvelope>& timing_envelopes() {
    static const std::map<std::string, TimingEnvelope> envelopes = {
        {"LOAD", {15000, 40000}},
        {"RESTRAINT_CHECK", {3000, 10000}},
        {"DISPATCH", {0, 2000}},
        {"RUN", {20000, 90000}},
        {"UNLOAD", {10000, 30000}},
    };
    return envelopes;
}

// The only phase legally allowed to precede DISPATCH. Encodes "the
// restraint check must be the last thing that happens before dispatch" as
// data rather than as a special case buried in the detection loop.
inline const std::string& required_predecessor_of_dispatch() {
    static const std::string p = "RESTRAINT_CHECK";
    return p;
}

// FRAMING_CHECKSUM, FRAMING_SEQUENCE and FRAMING_SYNC are the three
// framing-violation classes the framed serial-bus decoder below can
// raise; adding them to this one enum, rather than a parallel category
// field, is deliberate: `kind` already is the violation-category field
// the WPF grid and the diff harness key off of, so the framed path
// reuses it instead of widening the JSON schema for no reason.
enum class ViolationKind {
    TIMING_ENVELOPE,
    INTERLOCK,
    FRAMING_CHECKSUM,
    FRAMING_SEQUENCE,
    FRAMING_SYNC,
};

struct Violation {
    ViolationKind kind;
    std::string phase;
    int sample_index;   // the exact triggering row (CSV) or frame index (framed capture)
    long long value_ms; // the dwell (timing) or -1 (interlock / framing)
    std::string detail;
};

inline std::string to_string(ViolationKind k) {
    switch (k) {
        case ViolationKind::TIMING_ENVELOPE: return "TIMING_ENVELOPE";
        case ViolationKind::INTERLOCK: return "INTERLOCK";
        case ViolationKind::FRAMING_CHECKSUM: return "FRAMING_CHECKSUM";
        case ViolationKind::FRAMING_SEQUENCE: return "FRAMING_SEQUENCE";
        case ViolationKind::FRAMING_SYNC: return "FRAMING_SYNC";
    }
    return "UNKNOWN";
}

inline std::vector<TraceEvent> parse_trace_csv(const std::string& path) {
    std::vector<TraceEvent> events;
    std::ifstream in(path);
    std::string line;
    int index = 0;
    bool first = true;
    while (std::getline(in, line)) {
        // Python's csv module writes CRLF line terminators by default even
        // on a file opened with newline="" (see Findings in the README);
        // std::getline here splits on '\n' only, so a trailing '\r' would
        // otherwise silently ride along on every phase name and make every
        // map lookup and every "== DISPATCH" comparison fail without ever
        // throwing, which is exactly the bug this strip fixes.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (first && line.rfind("timestamp_ms", 0) == 0) { first = false; continue; }
        first = false;
        std::stringstream ss(line);
        std::string ts_str, phase;
        std::getline(ss, ts_str, ',');
        std::getline(ss, phase, ',');
        events.push_back({std::stoll(ts_str), phase, index});
        ++index;
    }
    return events;
}

// Reconstructs per-phase dwell times (the gap between one event and the
// next) and flags both violation kinds. A phase's dwell is measured as
// the time until the *next* event, so the violation, if any, is reported
// at the sample index of the event that ended the phase (the one where
// the overrun or underrun became knowable), not the one that started it.
inline std::vector<Violation> detect_violations(const std::vector<TraceEvent>& events) {
    std::vector<Violation> violations;
    const auto& envelopes = timing_envelopes();

    for (size_t i = 0; i + 1 < events.size(); ++i) {
        const auto& cur = events[i];
        const auto& next = events[i + 1];
        long long dwell = next.timestamp_ms - cur.timestamp_ms;

        auto it = envelopes.find(cur.phase);
        if (it != envelopes.end()) {
            if (dwell < it->second.min_ms || dwell > it->second.max_ms) {
                violations.push_back({ViolationKind::TIMING_ENVELOPE, cur.phase, next.sample_index,
                                       dwell,
                                       cur.phase + " dwelled " + std::to_string(dwell) +
                                           "ms, outside [" + std::to_string(it->second.min_ms) +
                                           ", " + std::to_string(it->second.max_ms) + "]"});
            }
        }

        if (next.phase == "DISPATCH" && cur.phase != required_predecessor_of_dispatch()) {
            violations.push_back({ViolationKind::INTERLOCK, "DISPATCH", next.sample_index, -1,
                                   "DISPATCH preceded by " + cur.phase + ", not " +
                                       required_predecessor_of_dispatch()});
        }
    }
    return violations;
}

// ---------------------------------------------------------------------
// Framed serial-bus capture support
//
// A framed serial-bus capture is a byte-oriented recording of the same
// controller cycle events as the CSV trace, but as they would actually
// arrive over a real bench serial bus: as discrete frames with framing
// fields, not as a text row per event. This is a second, independently
// interesting input format, not a re-encoding of the CSV path; a real
// framing decoder has to defend against corruption the CSV path never
// has to think about (a torn byte stream, a dropped byte, a flipped bit)
// which is exactly what the three framing-violation classes below exist
// to catch.
//
// Wire format, one frame:
//   byte 0            SYNC        0xAA
//   byte 1            LEN         payload length in bytes (always 5 here:
//                                  a 4-byte timestamp + a 1-byte phase code)
//   byte 2            SEQ         0-255, increments by 1 per frame, wraps
//   bytes 3..3+LEN-1  PAYLOAD     timestamp_ms as uint32 little-endian,
//                                  then a 1-byte phase code (index into
//                                  phase_code_table())
//   byte 3+LEN        CHECKSUM    additive: (LEN + SEQ + sum(PAYLOAD)) & 0xFF
//
// Checksum choice: a simple additive (mod-256) checksum was picked over
// a CRC-8. It is not as strong at catching multi-bit corruption, but it
// is transparent (the fault generator and this parser can both reason
// about it byte-by-byte without a polynomial table) and it is a real,
// common choice for a bench/debug serial link, which is the tool this
// resume entry is describing, not a safety-rated production bus.
inline constexpr unsigned char SERIAL_SYNC_BYTE = 0xAA;
inline constexpr int SERIAL_FRAME_PAYLOAD_LEN = 5; // 4-byte timestamp + 1-byte phase code

// The phase each payload's 1-byte phase code refers to. Index == code.
// Shared by the C++ generator and this parser (both compiled from this
// header); the Python reference re-declares the same table independently
// as part of the wire-format spec, the same way it re-declares the
// timing envelopes above.
inline const std::vector<std::string>& phase_code_table() {
    static const std::vector<std::string> table = {
        "LOAD", "RESTRAINT_CHECK", "DISPATCH", "RUN", "UNLOAD",
    };
    return table;
}

inline unsigned char compute_frame_checksum(unsigned char len, unsigned char seq,
                                             const std::vector<unsigned char>& payload) {
    unsigned int sum = static_cast<unsigned int>(len) + static_cast<unsigned int>(seq);
    for (unsigned char b : payload) sum += b;
    return static_cast<unsigned char>(sum & 0xFFu);
}

struct FramedParseResult {
    std::vector<TraceEvent> events;       // successfully decoded frames, as controller-cycle events
    std::vector<Violation> violations;    // framing violations only (checksum/sequence/sync)
    int frame_count = 0;                  // total frame slots walked, including corrupted ones
};

// Walks a raw byte capture frame by frame. On a bad checksum, records a
// FRAMING_CHECKSUM violation but still trusts the frame's own SEQ/PAYLOAD
// enough to keep decoding (a single corrupted byte should not blind the
// decoder to everything after it). On an unexpected sequence number,
// records FRAMING_SEQUENCE and resynchronizes its expectation to the
// value actually seen, so one skip is reported once, not forever. On a
// byte that is not the sync byte where a frame was expected, records
// FRAMING_SYNC ("loss of frame alignment") and scans forward for the
// next 0xAA to resume, exactly like a real decoder recovering from a
// torn byte stream would.
inline FramedParseResult parse_serial_frames(const std::string& path) {
    FramedParseResult result;
    std::ifstream in(path, std::ios::binary);
    std::vector<unsigned char> buf((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    const size_t n = buf.size();
    size_t pos = 0;
    int frame_index = 0;
    int expected_seq = -1; // unset until the first frame is seen
    const auto& phase_table = phase_code_table();

    while (pos < n) {
        if (buf[pos] != SERIAL_SYNC_BYTE) {
            size_t start = pos;
            while (pos < n && buf[pos] != SERIAL_SYNC_BYTE) ++pos;
            std::string where = pos < n ? ("resynced at byte " + std::to_string(pos))
                                         : "no further sync byte found, capture ends misaligned";
            result.violations.push_back({ViolationKind::FRAMING_SYNC, "FRAME", frame_index, -1,
                                          "lost sync at byte " + std::to_string(start) + ", " + where});
            ++frame_index;
            continue;
        }

        if (pos + 3 > n) {
            result.violations.push_back({ViolationKind::FRAMING_SYNC, "FRAME", frame_index, -1,
                                          "truncated frame header at byte " + std::to_string(pos)});
            ++result.frame_count;
            break;
        }
        unsigned char len_byte = buf[pos + 1];
        unsigned char seq_byte = buf[pos + 2];
        size_t frame_total = static_cast<size_t>(len_byte) + 4; // sync+len+seq+payload+checksum
        if (pos + frame_total > n) {
            result.violations.push_back({ViolationKind::FRAMING_SYNC, "FRAME", frame_index, -1,
                                          "truncated frame payload/checksum at byte " + std::to_string(pos)});
            ++result.frame_count;
            break;
        }

        std::vector<unsigned char> payload(buf.begin() + pos + 3, buf.begin() + pos + 3 + len_byte);
        unsigned char checksum_byte = buf[pos + 3 + len_byte];
        unsigned char computed = compute_frame_checksum(len_byte, seq_byte, payload);
        if (computed != checksum_byte) {
            result.violations.push_back({ViolationKind::FRAMING_CHECKSUM, "FRAME", frame_index, -1,
                                          "frame " + std::to_string(frame_index) +
                                              " checksum mismatch: computed " + std::to_string((int)computed) +
                                              ", got " + std::to_string((int)checksum_byte)});
        }

        if (expected_seq == -1) {
            expected_seq = seq_byte;
        } else if (seq_byte != (expected_seq & 0xFF)) {
            result.violations.push_back({ViolationKind::FRAMING_SEQUENCE, "FRAME", frame_index, -1,
                                          "frame " + std::to_string(frame_index) + " expected seq " +
                                              std::to_string(expected_seq & 0xFF) + ", got " +
                                              std::to_string((int)seq_byte)});
        }
        expected_seq = seq_byte + 1;

        if (payload.size() >= static_cast<size_t>(SERIAL_FRAME_PAYLOAD_LEN)) {
            uint32_t ts = static_cast<uint32_t>(payload[0]) | (static_cast<uint32_t>(payload[1]) << 8) |
                          (static_cast<uint32_t>(payload[2]) << 16) | (static_cast<uint32_t>(payload[3]) << 24);
            unsigned char phase_code = payload[4];
            std::string phase = phase_code < phase_table.size() ? phase_table[phase_code] : "UNKNOWN";
            result.events.push_back({static_cast<long long>(ts), phase, frame_index});
        }

        pos += frame_total;
        ++frame_index;
        ++result.frame_count;
    }
    return result;
}

// Reconstructs controller cycles from a framed serial-bus capture the
// same way parse_trace_csv + detect_violations does for the plain CSV
// path: decode frames into TraceEvents, then run the same timing-
// envelope/interlock rule engine over them, so a framed capture is
// checked for timing and interlock violations exactly as thoroughly as
// a CSV one, in addition to the framing violations the byte layer itself
// can raise. This is the one function that answers "flagging timing-
// envelope, interlock and framing violations by exact triggering sample"
// for the framed-capture path.
inline std::vector<Violation> detect_framed_violations(const std::string& path) {
    FramedParseResult parsed = parse_serial_frames(path);
    std::vector<Violation> all = parsed.violations;
    std::vector<Violation> trace_violations = detect_violations(parsed.events);
    all.insert(all.end(), trace_violations.begin(), trace_violations.end());
    return all;
}
