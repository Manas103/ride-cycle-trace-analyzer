#include <cstdio>
#include <cstdlib>
#include <fstream>

#include "trace_engine.hpp"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        }                                                                     \
    } while (0)

static std::vector<TraceEvent> make_clean_cycle() {
    return {
        {0, "LOAD", 0},
        {20000, "RESTRAINT_CHECK", 1},
        {25000, "DISPATCH", 2},
        {26000, "RUN", 3},
        {70000, "UNLOAD", 4},
        {85000, "LOAD", 5},
    };
}

static void test_clean_cycle_has_no_violations() {
    auto violations = detect_violations(make_clean_cycle());
    CHECK(violations.empty(), "a clean, in-envelope, correctly-ordered cycle has 0 violations");
}

static void test_timing_envelope_overrun_detected() {
    auto events = make_clean_cycle();
    events[1].timestamp_ms = 50000; // LOAD dwelled 50s, over the 40s max
    auto violations = detect_violations(events);
    bool found = false;
    for (const auto& v : violations) {
        if (v.kind == ViolationKind::TIMING_ENVELOPE && v.phase == "LOAD" && v.sample_index == 1) {
            found = true;
        }
    }
    CHECK(found, "an over-long LOAD dwell is flagged at the exact sample index that revealed it");
}

static void test_timing_envelope_underrun_detected() {
    auto events = make_clean_cycle();
    events[1].timestamp_ms = 5000; // LOAD dwelled 5s, under the 15s min
    auto violations = detect_violations(events);
    bool found = false;
    for (const auto& v : violations) {
        if (v.kind == ViolationKind::TIMING_ENVELOPE && v.phase == "LOAD") found = true;
    }
    CHECK(found, "an under-long LOAD dwell is also flagged (envelope has both a floor and a ceiling)");
}

static void test_interlock_violation_dispatch_without_restraint_check() {
    std::vector<TraceEvent> events = {
        {0, "LOAD", 0},
        {20000, "DISPATCH", 1}, // skipped RESTRAINT_CHECK entirely
        {21000, "RUN", 2},
    };
    auto violations = detect_violations(events);
    bool found = false;
    for (const auto& v : violations) {
        if (v.kind == ViolationKind::INTERLOCK && v.sample_index == 1) found = true;
    }
    CHECK(found, "dispatching straight from LOAD without a restraint check is an interlock violation");
}

static void test_exact_triggering_sample_index() {
    // A violation later in a longer trace must still name the precise row
    // that revealed it, not just "somewhere in the trace".
    std::vector<TraceEvent> events = make_clean_cycle();
    events.push_back({105000, "RESTRAINT_CHECK", 6});
    events.push_back({106000, "DISPATCH", 7});
    events.push_back({200000, "RUN", 8}); // DISPATCH dwelled 94s, way over its 2s max
    auto violations = detect_violations(events);
    bool found_at_8 = false;
    for (const auto& v : violations) {
        if (v.phase == "DISPATCH" && v.sample_index == 8) found_at_8 = true;
    }
    CHECK(found_at_8, "a violation deep in a trace still names the exact row, not an approximate one");
}

static void test_csv_round_trip(const std::string& tmp_path) {
    std::ofstream out(tmp_path);
    out << "timestamp_ms,phase\n0,LOAD\n20000,RESTRAINT_CHECK\n25000,DISPATCH\n26000,RUN\n";
    out.close();
    auto events = parse_trace_csv(tmp_path);
    CHECK(events.size() == 4, "CSV parser reads all 4 data rows, skipping the header");
    CHECK(events[0].phase == "LOAD" && events[0].timestamp_ms == 0, "first row parsed correctly");
    CHECK(events[2].phase == "DISPATCH" && events[2].timestamp_ms == 25000,
          "third row parsed correctly");
}

int main() {
    test_clean_cycle_has_no_violations();
    test_timing_envelope_overrun_detected();
    test_timing_envelope_underrun_detected();
    test_interlock_violation_dispatch_without_restraint_check();
    test_exact_triggering_sample_index();
    test_csv_round_trip("test_trace_tmp.csv");
    std::remove("test_trace_tmp.csv");

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
