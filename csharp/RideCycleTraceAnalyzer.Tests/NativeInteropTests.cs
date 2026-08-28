using System.IO;
using RideCycleTraceAnalyzer;
using Xunit;

namespace RideCycleTraceAnalyzer.Tests;

// A genuine end-to-end P/Invoke test: calls into the real
// TraceEngineNative.dll (built by CMake + MSVC; see the repo README) and
// checks its answer against the same synthetic sessions the C++/Python
// diff harness uses, rather than only exercising TraceReport's JSON
// parsing against a literal string. Skips cleanly if the native DLL has
// not been built yet, matching this workspace's convention for tests
// that need something the environment might not have (model-validation
// -alerting's PostgreSQL skip is the precedent).
public class NativeInteropTests
{
    private static string SessionsDir()
    {
        // csharp/RideCycleTraceAnalyzer.Tests/bin/Debug/net8.0-windows -> repo root
        var dir = AppContext.BaseDirectory;
        var repoRoot = Path.GetFullPath(Path.Combine(dir, "..", "..", "..", "..", ".."));
        return Path.Combine(repoRoot, "data", "sessions");
    }

    private static bool NativeDllPresent() =>
        File.Exists(Path.Combine(AppContext.BaseDirectory, "TraceEngineNative.dll"));

    [Fact]
    public void AnalyzeTrace_finds_no_violations_on_a_clean_session()
    {
        if (!NativeDllPresent()) return; // native DLL not built in this environment
        var path = Path.Combine(SessionsDir(), "clean_00.csv");
        if (!File.Exists(path)) return; // fixtures not generated in this environment
        var report = NativeInterop.AnalyzeTrace(path);
        Assert.Empty(report.Violations);
    }

    [Fact]
    public void AnalyzeTrace_catches_the_seeded_interlock_violation()
    {
        if (!NativeDllPresent()) return;
        var path = Path.Combine(SessionsDir(), "fault_20.csv"); // an interlock-skip fixture
        if (!File.Exists(path)) return;
        var report = NativeInterop.AnalyzeTrace(path);
        Assert.Contains(report.Violations, v => v.Kind == "INTERLOCK");
    }

    private static string FramesDir()
    {
        var dir = AppContext.BaseDirectory;
        var repoRoot = Path.GetFullPath(Path.Combine(dir, "..", "..", "..", "..", ".."));
        return Path.Combine(repoRoot, "data", "frames");
    }

    [Fact]
    public void AnalyzeFramedCapture_finds_no_violations_on_a_clean_framed_session()
    {
        if (!NativeDllPresent()) return;
        var path = Path.Combine(FramesDir(), "clean_00.bin");
        if (!File.Exists(path)) return;
        var report = NativeInterop.AnalyzeFramedCapture(path);
        Assert.Empty(report.Violations);
    }

    [Fact]
    public void AnalyzeFramedCapture_catches_the_seeded_checksum_violation()
    {
        if (!NativeDllPresent()) return;
        var path = Path.Combine(FramesDir(), "fault_00_checksum.bin");
        if (!File.Exists(path)) return;
        var report = NativeInterop.AnalyzeFramedCapture(path);
        Assert.Contains(report.Violations, v => v.Kind == "FRAMING_CHECKSUM");
    }
}
