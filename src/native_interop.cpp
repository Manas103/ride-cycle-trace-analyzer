// The P/Invoke surface the WPF app calls into. Built only on Windows
// (see CMakeLists.txt): __declspec(dllexport) and __stdcall are
// Windows/MSVC conventions, and this DLL exists purely so the desktop
// app does not have to shell out to a separate process to parse a trace.
// The parsing logic itself is the same trace_engine.hpp the CLI and the
// hand-rolled tests use; this file is a thin marshaling layer, not a
// second implementation.
#include <cstring>
#include <sstream>

#include "trace_engine.hpp"

// Builds the same TraceReport JSON shape (event_count, violations,
// phases) from a decoded event/violation pair. Both AnalyzeTraceFile
// (CSV path) and AnalyzeFramedCaptureFile (framed serial-bus path) call
// this, so the WPF grid and chart do not need to know or care which
// capture format produced the report; a FRAMING_* kind string is just
// another value in the same "kind" column the grid already renders.
static std::string build_report_json(const std::vector<TraceEvent>& events,
                                      const std::vector<Violation>& violations) {
    std::ostringstream json;
    json << "{\"event_count\":" << events.size() << ",\"violations\":[";
    for (size_t i = 0; i < violations.size(); ++i) {
        const auto& v = violations[i];
        if (i) json << ",";
        json << "{\"kind\":\"" << to_string(v.kind) << "\","
             << "\"phase\":\"" << v.phase << "\","
             << "\"sample_index\":" << v.sample_index << ","
             << "\"value_ms\":" << v.value_ms << ","
             << "\"detail\":\"" << v.detail << "\"}";
    }
    json << "],\"phases\":[";
    for (size_t i = 0; i + 1 < events.size(); ++i) {
        if (i) json << ",";
        long long dwell = events[i + 1].timestamp_ms - events[i].timestamp_ms;
        json << "{\"phase\":\"" << events[i].phase << "\",\"dwell_ms\":" << dwell << "}";
    }
    json << "]}";
    return json.str();
}

static int write_out(const std::string& s, char* out_buf, int out_buf_capacity) {
    if (static_cast<int>(s.size()) + 1 > out_buf_capacity) {
        return -1;
    }
    std::memcpy(out_buf, s.c_str(), s.size() + 1);
    return static_cast<int>(s.size());
}

extern "C" {

// Writes a JSON summary of the CSV trace at `path` into `out_buf`.
// Returns the number of bytes written (excluding the null terminator) on
// success, -1 if `out_buf_capacity` was too small (the caller should
// retry with a larger buffer), or -2 if the file could not be read.
__declspec(dllexport) int __stdcall AnalyzeTraceFile(const char* path, char* out_buf,
                                                      int out_buf_capacity) {
    std::vector<TraceEvent> events;
    try {
        events = parse_trace_csv(path);
    } catch (...) {
        return -2;
    }
    auto violations = detect_violations(events);
    return write_out(build_report_json(events, violations), out_buf, out_buf_capacity);
}

// Same contract as AnalyzeTraceFile, for a framed serial-bus capture
// (see include/trace_engine.hpp for the wire format). Decoded events are
// reconstructed from frame payloads and checked for timing-envelope and
// interlock violations exactly as the CSV path is, in addition to the
// framing violations (FRAMING_CHECKSUM, FRAMING_SEQUENCE, FRAMING_SYNC)
// the byte layer itself can raise.
__declspec(dllexport) int __stdcall AnalyzeFramedCaptureFile(const char* path, char* out_buf,
                                                               int out_buf_capacity) {
    FramedParseResult parsed;
    try {
        parsed = parse_serial_frames(path);
    } catch (...) {
        return -2;
    }
    std::vector<Violation> all = parsed.violations;
    auto trace_violations = detect_violations(parsed.events);
    all.insert(all.end(), trace_violations.begin(), trace_violations.end());
    return write_out(build_report_json(parsed.events, all), out_buf, out_buf_capacity);
}
}
