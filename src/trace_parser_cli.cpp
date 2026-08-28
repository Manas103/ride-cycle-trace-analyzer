// Reads a raw controller trace CSV (or, with --framed, a framed
// serial-bus capture) and writes the violation list as CSV. This is the
// exact command the diff harness runs to get the C++ side's answer for
// comparison against the independent Python reference.
#include <cstdio>
#include <string>

#include "trace_engine.hpp"

static void write_violations_csv(const std::string& out_path, const std::vector<Violation>& violations) {
    std::FILE* f = std::fopen(out_path.c_str(), "w");
    if (!f) {
        std::fprintf(stderr, "could not open %s for writing\n", out_path.c_str());
        std::exit(1);
    }
    std::fprintf(f, "kind,phase,sample_index,value_ms,detail\n");
    for (const auto& v : violations) {
        std::fprintf(f, "%s,%s,%d,%lld,%s\n", to_string(v.kind).c_str(), v.phase.c_str(),
                     v.sample_index, v.value_ms, v.detail.c_str());
    }
    std::fclose(f);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                      "usage: trace_parser_cli <trace.csv> [violations_out.csv]\n"
                      "       trace_parser_cli --framed <capture.bin> [violations_out.csv]\n");
        return 2;
    }

    bool framed = std::string(argv[1]) == "--framed";
    if (framed && argc < 3) {
        std::fprintf(stderr, "usage: trace_parser_cli --framed <capture.bin> [violations_out.csv]\n");
        return 2;
    }

    if (framed) {
        std::string capture_path = argv[2];
        std::string out_path = argc > 3 ? argv[3] : "violations.csv";
        auto parsed = parse_serial_frames(capture_path);
        auto all = detect_framed_violations(capture_path);

        write_violations_csv(out_path, all);
        std::printf("%s: %d frames, %zu events, %zu violations\n", capture_path.c_str(),
                    parsed.frame_count, parsed.events.size(), all.size());
        return 0;
    }

    std::string trace_path = argv[1];
    std::string out_path = argc > 2 ? argv[2] : "violations.csv";

    auto events = parse_trace_csv(trace_path);
    auto violations = detect_violations(events);

    write_violations_csv(out_path, violations);
    std::printf("%s: %zu events, %zu violations\n", trace_path.c_str(), events.size(),
                violations.size());
    return 0;
}
