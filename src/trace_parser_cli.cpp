// Reads a raw controller trace CSV and writes the violation list as CSV.
// This is the exact command the diff harness runs to get the C++ side's
// answer for comparison against the independent Python reference.
#include <cstdio>
#include <string>

#include "trace_engine.hpp"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: trace_parser_cli <trace.csv> [violations_out.csv]\n");
        return 2;
    }
    std::string trace_path = argv[1];
    std::string out_path = argc > 2 ? argv[2] : "violations.csv";

    auto events = parse_trace_csv(trace_path);
    auto violations = detect_violations(events);

    std::FILE* f = std::fopen(out_path.c_str(), "w");
    if (!f) {
        std::fprintf(stderr, "could not open %s for writing\n", out_path.c_str());
        return 1;
    }
    std::fprintf(f, "kind,phase,sample_index,value_ms,detail\n");
    for (const auto& v : violations) {
        std::fprintf(f, "%s,%s,%d,%lld,%s\n", to_string(v.kind).c_str(), v.phase.c_str(),
                     v.sample_index, v.value_ms, v.detail.c_str());
    }
    std::fclose(f);

    std::printf("%s: %zu events, %zu violations\n", trace_path.c_str(), events.size(),
                violations.size());
    return 0;
}
