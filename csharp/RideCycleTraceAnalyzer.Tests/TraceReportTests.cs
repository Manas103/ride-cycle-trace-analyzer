using RideCycleTraceAnalyzer;
using Xunit;

namespace RideCycleTraceAnalyzer.Tests;

public class TraceReportTests
{
    private const string SampleJson = """
    {
      "event_count": 3,
      "violations": [
        {"kind": "TIMING_ENVELOPE", "phase": "LOAD", "sample_index": 1, "value_ms": 43465, "detail": "LOAD dwelled 43465ms, outside [15000, 40000]"}
      ],
      "phases": [
        {"phase": "LOAD", "dwell_ms": 43465},
        {"phase": "RESTRAINT_CHECK", "dwell_ms": 5000}
      ]
    }
    """;

    [Fact]
    public void FromJson_reads_event_count()
    {
        var report = TraceReport.FromJson(SampleJson);
        Assert.Equal(3, report.EventCount);
    }

    [Fact]
    public void FromJson_reads_violations_with_exact_triggering_sample_index()
    {
        var report = TraceReport.FromJson(SampleJson);
        var v = Assert.Single(report.Violations);
        Assert.Equal("TIMING_ENVELOPE", v.Kind);
        Assert.Equal("LOAD", v.Phase);
        Assert.Equal(1, v.SampleIndex);
        Assert.Equal(43465, v.ValueMs);
    }

    [Fact]
    public void FromJson_reads_phase_dwells_in_order()
    {
        var report = TraceReport.FromJson(SampleJson);
        Assert.Equal(2, report.Phases.Count);
        Assert.Equal("LOAD", report.Phases[0].Phase);
        Assert.Equal(43465, report.Phases[0].DwellMs);
        Assert.Equal("RESTRAINT_CHECK", report.Phases[1].Phase);
    }

    [Fact]
    public void FromJson_handles_zero_violations()
    {
        const string clean = """{"event_count": 2, "violations": [], "phases": [{"phase": "LOAD", "dwell_ms": 20000}]}""";
        var report = TraceReport.FromJson(clean);
        Assert.Empty(report.Violations);
        Assert.Single(report.Phases);
    }
}
