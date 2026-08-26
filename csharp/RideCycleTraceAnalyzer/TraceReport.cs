using System.Text.Json;
using System.Text.Json.Serialization;

namespace RideCycleTraceAnalyzer;

public record PhaseDwell(string Phase, long DwellMs);

public record Violation(string Kind, string Phase, int SampleIndex, long ValueMs, string Detail);

public record TraceReport(int EventCount, List<Violation> Violations, List<PhaseDwell> Phases)
{
    // Parses the JSON the native DLL's AnalyzeTraceFile writes. Kept as a
    // static method on the model rather than inline in the P/Invoke
    // wrapper so it can be unit tested against a literal JSON string,
    // without touching the native DLL at all.
    public static TraceReport FromJson(string json)
    {
        var options = new JsonSerializerOptions
        {
            PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        };
        var raw = JsonSerializer.Deserialize<RawReport>(json, options)
                  ?? throw new JsonException("null report");
        var violations = raw.Violations
            .Select(v => new Violation(v.Kind, v.Phase, v.SampleIndex, v.ValueMs, v.Detail))
            .ToList();
        var phases = raw.Phases
            .Select(p => new PhaseDwell(p.Phase, p.DwellMs))
            .ToList();
        return new TraceReport(raw.EventCount, violations, phases);
    }

    private record RawReport(
        [property: JsonPropertyName("event_count")] int EventCount,
        [property: JsonPropertyName("violations")] List<RawViolation> Violations,
        [property: JsonPropertyName("phases")] List<RawPhase> Phases);

    private record RawViolation(
        [property: JsonPropertyName("kind")] string Kind,
        [property: JsonPropertyName("phase")] string Phase,
        [property: JsonPropertyName("sample_index")] int SampleIndex,
        [property: JsonPropertyName("value_ms")] long ValueMs,
        [property: JsonPropertyName("detail")] string Detail);

    private record RawPhase(
        [property: JsonPropertyName("phase")] string Phase,
        [property: JsonPropertyName("dwell_ms")] long DwellMs);
}
