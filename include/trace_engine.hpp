#pragma once
#include <fstream>
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

enum class ViolationKind {
    TIMING_ENVELOPE,
    INTERLOCK,
};

struct Violation {
    ViolationKind kind;
    std::string phase;
    int sample_index;   // the exact triggering row
    long long value_ms; // the dwell (timing) or -1 (interlock)
    std::string detail;
};

inline std::string to_string(ViolationKind k) {
    return k == ViolationKind::TIMING_ENVELOPE ? "TIMING_ENVELOPE" : "INTERLOCK";
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
