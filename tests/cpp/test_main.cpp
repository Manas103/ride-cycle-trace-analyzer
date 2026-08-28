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

// --- framed serial-bus capture tests -------------------------------

static void append_frame(std::vector<unsigned char>& buf, unsigned char seq, uint32_t ts,
                          const std::string& phase, bool corrupt_checksum = false,
                          bool corrupt_sync = false) {
    const auto& table = phase_code_table();
    unsigned char phase_code = 0;
    for (size_t i = 0; i < table.size(); ++i) {
        if (table[i] == phase) { phase_code = static_cast<unsigned char>(i); break; }
    }
    std::vector<unsigned char> payload = {
        static_cast<unsigned char>(ts & 0xFF), static_cast<unsigned char>((ts >> 8) & 0xFF),
        static_cast<unsigned char>((ts >> 16) & 0xFF), static_cast<unsigned char>((ts >> 24) & 0xFF),
        phase_code,
    };
    unsigned char len = static_cast<unsigned char>(payload.size());
    unsigned char checksum = compute_frame_checksum(len, seq, payload);
    if (corrupt_checksum) checksum ^= 0xFF;
    buf.push_back(corrupt_sync ? static_cast<unsigned char>(0x00) : SERIAL_SYNC_BYTE);
    buf.push_back(len);
    buf.push_back(seq);
    buf.insert(buf.end(), payload.begin(), payload.end());
    buf.push_back(checksum);
}

static void write_binary_file(const std::string& path, const std::vector<unsigned char>& buf) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
}

static void test_framed_clean_capture_has_no_violations(const std::string& tmp_path) {
    std::vector<unsigned char> buf;
    append_frame(buf, 0, 0, "LOAD");
    append_frame(buf, 1, 20000, "RESTRAINT_CHECK");
    append_frame(buf, 2, 25000, "DISPATCH");
    append_frame(buf, 3, 26000, "RUN");
    append_frame(buf, 4, 70000, "UNLOAD");
    write_binary_file(tmp_path, buf);
    auto violations = detect_framed_violations(tmp_path);
    CHECK(violations.empty(), "a clean, correctly-framed, in-envelope capture has 0 violations");
}

static void test_framed_checksum_violation_detected(const std::string& tmp_path) {
    std::vector<unsigned char> buf;
    append_frame(buf, 0, 0, "LOAD");
    append_frame(buf, 1, 20000, "RESTRAINT_CHECK", /*corrupt_checksum=*/true);
    append_frame(buf, 2, 25000, "DISPATCH");
    write_binary_file(tmp_path, buf);
    auto violations = detect_framed_violations(tmp_path);
    bool found = false;
    for (const auto& v : violations) {
        if (v.kind == ViolationKind::FRAMING_CHECKSUM && v.sample_index == 1) found = true;
    }
    CHECK(found, "a corrupted checksum byte is flagged at its exact frame index");
}

static void test_framed_sequence_violation_detected(const std::string& tmp_path) {
    std::vector<unsigned char> buf;
    append_frame(buf, 0, 0, "LOAD");
    append_frame(buf, 5, 20000, "RESTRAINT_CHECK"); // should have been seq 1, jumped to 5
    append_frame(buf, 6, 25000, "DISPATCH");
    write_binary_file(tmp_path, buf);
    auto violations = detect_framed_violations(tmp_path);
    bool found = false;
    for (const auto& v : violations) {
        if (v.kind == ViolationKind::FRAMING_SEQUENCE && v.sample_index == 1) found = true;
    }
    CHECK(found, "a skipped sequence number is flagged at its exact frame index");
}

static void test_framed_sync_loss_detected(const std::string& tmp_path) {
    std::vector<unsigned char> buf;
    append_frame(buf, 0, 0, "LOAD");
    append_frame(buf, 1, 20000, "RESTRAINT_CHECK", /*corrupt_checksum=*/false, /*corrupt_sync=*/true);
    append_frame(buf, 2, 25000, "DISPATCH");
    write_binary_file(tmp_path, buf);
    auto violations = detect_framed_violations(tmp_path);
    bool found = false;
    for (const auto& v : violations) {
        if (v.kind == ViolationKind::FRAMING_SYNC && v.sample_index == 1) found = true;
    }
    CHECK(found, "a corrupted sync byte is flagged as a loss of frame alignment at its frame index");
    // The decoder must recover: the frame after the corrupted one should
    // still be readable once it resynchronizes on the next 0xAA.
    bool recovered = false;
    for (const auto& e : parse_serial_frames(tmp_path).events) {
        if (e.phase == "DISPATCH") recovered = true;
    }
    CHECK(recovered, "the decoder resynchronizes and keeps reading frames after a sync loss");
}

static void test_framed_reconstructs_timing_and_interlock_violations(const std::string& tmp_path) {
    // A framed capture must be checked for timing-envelope and interlock
    // violations exactly as thoroughly as the CSV path, not just framing
    // ones, per the "reconstructs controller cycles ... flagging timing-
    // envelope, interlock and framing violations" claim.
    std::vector<unsigned char> buf;
    append_frame(buf, 0, 0, "LOAD");
    append_frame(buf, 1, 20000, "DISPATCH"); // interlock: skipped RESTRAINT_CHECK
    append_frame(buf, 2, 21000, "RUN");
    write_binary_file(tmp_path, buf);
    auto violations = detect_framed_violations(tmp_path);
    bool found_interlock = false;
    for (const auto& v : violations) {
        if (v.kind == ViolationKind::INTERLOCK && v.sample_index == 1) found_interlock = true;
    }
    CHECK(found_interlock, "an interlock violation inside a framed capture's decoded events is still caught");
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

    test_framed_clean_capture_has_no_violations("test_frame_tmp.bin");
    test_framed_checksum_violation_detected("test_frame_tmp.bin");
    test_framed_sequence_violation_detected("test_frame_tmp.bin");
    test_framed_sync_loss_detected("test_frame_tmp.bin");
    test_framed_reconstructs_timing_and_interlock_violations("test_frame_tmp.bin");
    std::remove("test_frame_tmp.bin");

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
