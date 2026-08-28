using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Shapes;
using Microsoft.Win32;

namespace RideCycleTraceAnalyzer;

public partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();
    }

    private void OpenButton_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog
        {
            Filter = "Trace CSV (*.csv)|*.csv|Framed serial-bus capture (*.bin)|*.bin",
        };
        if (dialog.ShowDialog() != true) return;

        try
        {
            // A .bin capture is the framed serial-bus format (see
            // include/trace_engine.hpp); everything else is treated as
            // the plain CSV trace. Both paths return the same TraceReport
            // shape, so the grid and chart below do not need to branch.
            bool framed = System.IO.Path.GetExtension(dialog.FileName).Equals(".bin", StringComparison.OrdinalIgnoreCase);
            TraceReport report = framed
                ? NativeInterop.AnalyzeFramedCapture(dialog.FileName)
                : NativeInterop.AnalyzeTrace(dialog.FileName);
            PhaseGrid.ItemsSource = report.Phases;
            ViolationGrid.ItemsSource = report.Violations;
            DrawDwellChart(report.Phases);
            StatusText.Text = $"{report.EventCount} events, {report.Violations.Count} violation(s)";
        }
        catch (Exception ex)
        {
            StatusText.Text = $"Error: {ex.Message}";
        }
    }

    private void DrawDwellChart(List<PhaseDwell> phases)
    {
        DwellChart.Children.Clear();
        if (phases.Count == 0) return;

        double chartWidth = Math.Max(DwellChart.ActualWidth, 800);
        double chartHeight = 180;
        double barWidth = chartWidth / phases.Count;
        long maxDwell = phases.Max(p => p.DwellMs);
        if (maxDwell <= 0) maxDwell = 1;

        for (int i = 0; i < phases.Count; i++)
        {
            double barHeight = (double)phases[i].DwellMs / maxDwell * (chartHeight - 20);
            var rect = new Rectangle
            {
                Width = barWidth - 2,
                Height = barHeight,
                Fill = Brushes.SteelBlue,
            };
            Canvas.SetLeft(rect, i * barWidth);
            Canvas.SetTop(rect, chartHeight - barHeight);
            DwellChart.Children.Add(rect);
        }
    }
}
