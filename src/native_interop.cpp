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

extern "C" {

// Writes a JSON summary of the trace at `path` into `out_buf`. Returns
// the number of bytes written (excluding the null terminator) on
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

    std::string s = json.str();
    if (static_cast<int>(s.size()) + 1 > out_buf_capacity) {
        return -1;
    }
    std::memcpy(out_buf, s.c_str(), s.size() + 1);
    return static_cast<int>(s.size());
}
}
