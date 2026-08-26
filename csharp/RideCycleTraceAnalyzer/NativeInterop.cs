using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace RideCycleTraceAnalyzer;

// P/Invoke surface into TraceEngineNative.dll (built separately by CMake
// + MSVC from the same include/trace_engine.hpp the CLI and the C++ test
// suite use; see README "Building and running"). This class does the
// marshaling and the growing-buffer retry; TraceReport.FromJson does the
// actual parsing, so that logic can be unit tested without the DLL.
public static class NativeInterop
{
    [DllImport("TraceEngineNative.dll", CallingConvention = CallingConvention.StdCall,
        CharSet = CharSet.Ansi)]
    private static extern int AnalyzeTraceFile(string path, StringBuilder outBuf, int outBufCapacity);

    public static TraceReport AnalyzeTrace(string path)
    {
        int capacity = 4096;
        while (true)
        {
            var buf = new StringBuilder(capacity);
            int result = AnalyzeTraceFile(path, buf, capacity);
            if (result == -2) throw new IOException($"native engine could not read {path}");
            if (result == -1) { capacity *= 2; continue; }
            return TraceReport.FromJson(buf.ToString());
        }
    }
}
