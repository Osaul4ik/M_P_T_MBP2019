using System;
using System.IO;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Shapes;
using AmtPtpConfigGui.Native;
using Microsoft.Win32;

namespace AmtPtpConfigGui
{
    public partial class MainWindow : Window
    {
        private readonly DeviceIo _device = new DeviceIo();
        private readonly System.Collections.Generic.List<string> _diagnosticLog = new();
        private PadGeometry _geometry = PadGeometry.Fallback;
        private bool _geometryFromDevice;
        private bool _suppressEvents;
        private bool _draggingTestPoint;
        private bool _uiReady;

        public MainWindow()
        {
            InitializeComponent();
            _uiReady = true;
            Loaded += (_, _) => Reconnect();
        }

        // ---------------------------------------------------------------
        // Connection handling
        // ---------------------------------------------------------------

        private void Reconnect_Click(object sender, RoutedEventArgs e) => Reconnect();

        private void SaveErrors_Click(object sender, RoutedEventArgs e)
        {
            if (_diagnosticLog.Count == 0)
            {
                MessageBox.Show(
                    "Ще немає жодної зафіксованої помилки підключення — спершу натисніть \"Оновити / Reconnect\".",
                    "Немає даних", MessageBoxButton.OK, MessageBoxImage.Information);
                return;
            }

            var dialog = new SaveFileDialog
            {
                Title = "Зберегти журнал помилок",
                Filter = "Текстові файли (*.txt)|*.txt|Усі файли (*.*)|*.*",
                FileName = $"AmtPtpConfigGui_errors_{DateTime.Now:yyyyMMdd_HHmmss}.txt"
            };

            if (dialog.ShowDialog(this) != true)
                return;

            try
            {
                File.WriteAllLines(dialog.FileName, _diagnosticLog);
                SetBottomStatus($"Журнал помилок збережено: {dialog.FileName}");
            }
            catch (Exception ex)
            {
                MessageBox.Show(
                    $"Не вдалося зберегти файл:\n{ex.Message}",
                    "Помилка збереження", MessageBoxButton.OK, MessageBoxImage.Error);
            }
        }

        private void Reconnect()
        {
            bool connected = _device.TryConnect();

            if (connected)
            {
                StatusDot.Fill = new SolidColorBrush(Color.FromRgb(0x2E, 0xA0, 0x4A));
                StatusText.Text = "Підключено: Wellspring Precision Touchpad";

                if (_device.TryGetPalmConfig(out var cfg))
                {
                    LoadConfigIntoSliders(cfg);
                }
                else
                {
                    LoadConfigIntoSliders(PalmConfig.Default);
                    SetBottomStatus("Пристрій знайдено, але не вдалося прочитати конфігурацію — показано значення за замовчуванням.");
                }

                if (_device.TryGetPadGeometry(out var geo))
                {
                    _geometry = geo;
                    _geometryFromDevice = true;
                }
                else
                {
                    _geometry = PadGeometry.Fallback;
                    _geometryFromDevice = false;
                }
            }
            else
            {
                StatusDot.Fill = new SolidColorBrush(Color.FromRgb(0xC0, 0x39, 0x2B));
                StatusText.Text = "Пристрій не знайдено — режим попереднього перегляду";
                LoadConfigIntoSliders(PalmConfig.Default);
                _geometry = PadGeometry.Fallback;
                _geometryFromDevice = false;

                // Surface exactly which SetupAPI/CreateFile step failed and
                // why, right in the GUI - no debugger or Event Viewer needed.
                SetBottomStatus(string.IsNullOrEmpty(_device.LastErrorMessage)
                    ? ""
                    : $"Діагностика: {_device.LastErrorMessage}");

                if (!string.IsNullOrEmpty(_device.LastErrorMessage))
                {
                    _diagnosticLog.Add($"[{DateTime.Now:yyyy-MM-dd HH:mm:ss}] {_device.LastErrorMessage}");
                }
            }

            GeometrySourceText.Text = _geometryFromDevice
                ? "геометрія: з пристрою"
                : "геометрія: приблизна (пристрій не підключено)";

            DrawPreview();
        }

        // ---------------------------------------------------------------
        // Slider <-> PalmConfig plumbing
        // ---------------------------------------------------------------

        private void LoadConfigIntoSliders(PalmConfig cfg)
        {
            _suppressEvents = true;
            try
            {
                SlEdgeTop.Value = cfg.EdgePermilleTop;
                SlEdgeLeft.Value = cfg.EdgePermilleLeft;
                SlEdgeRight.Value = cfg.EdgePermilleRight;
                SlEdgeBottom.Value = cfg.EdgePermilleBottom;

                SlLargeMajor.Value = cfg.PalmLargeMajor;
                SlLargeRatio.Value = cfg.PalmLargeRatio;
                SlScoreThresh.Value = cfg.PalmScoreThresh;
                SlMinMajor.Value = cfg.PalmMinMajor;
                SlMinMinor.Value = cfg.PalmMinMinor;
            }
            finally
            {
                _suppressEvents = false;
            }
            UpdateAllLabels();
        }

        private PalmConfig ReadConfigFromSliders()
        {
            var c = PalmConfig.Default;
            c.EdgePermilleTop = (uint)SlEdgeTop.Value;
            c.EdgePermilleLeft = (uint)SlEdgeLeft.Value;
            c.EdgePermilleRight = (uint)SlEdgeRight.Value;
            c.EdgePermilleBottom = (uint)SlEdgeBottom.Value;
            c.PalmLargeMajor = (uint)SlLargeMajor.Value;
            c.PalmLargeRatio = (uint)SlLargeRatio.Value;
            c.PalmScoreThresh = (uint)SlScoreThresh.Value;
            c.PalmMinMajor = (uint)SlMinMajor.Value;
            c.PalmMinMinor = (uint)SlMinMinor.Value;
            return c.Clamped();
        }

        private void UpdateAllLabels()
        {
            LblEdgeTop.Text = FormatPermille(SlEdgeTop.Value);
            LblEdgeLeft.Text = FormatPermille(SlEdgeLeft.Value);
            LblEdgeRight.Text = FormatPermille(SlEdgeRight.Value);
            LblEdgeBottom.Text = FormatPermille(SlEdgeBottom.Value);

            LblLargeMajor.Text = $"{SlLargeMajor.Value:0}";
            LblLargeRatio.Text = $"{SlLargeRatio.Value / 100.0:0.00}×";
            LblScoreThresh.Text = $"{SlScoreThresh.Value:0}";
            LblMinMajor.Text = $"{SlMinMajor.Value:0}";
            LblMinMinor.Text = $"{SlMinMinor.Value:0}";

            LblTestMajor.Text = $"{SlTestMajor.Value:0}";
            LblTestMinor.Text = $"{SlTestMinor.Value:0}";
            LblTestX.Text = $"{SlTestX.Value:0}%";
            LblTestY.Text = $"{SlTestY.Value:0}%";
        }

        private static string FormatPermille(double permille) => $"{permille / 10.0:0.0}%";

        private void Slider_Changed(object sender, RoutedEventArgs e)
        {
            // Setting Minimum/Maximum on sliders declared earlier in the XAML
            // can coerce Value and fire this handler while InitializeComponent()
            // is still parsing later controls (e.g. SlLargeMajor's Min/Max is
            // set before SlLargeRatio exists) — those fields are still null then.
            if (!_uiReady || _suppressEvents) return;
            UpdateAllLabels();
            DrawPreview();
        }

        // ---------------------------------------------------------------
        // Live preview canvas
        //
        // Draws (a) the pad outline to scale, (b) the four shaded edge-zone
        // bands sized by the current permille sliders, and (c) an ellipse
        // representing the test touch's Major/Minor axis at the chosen
        // X/Y position, color-coded by the same classification the driver
        // would produce (PalmPreviewEngine mirrors Palm.c exactly).
        // ---------------------------------------------------------------

        private void DrawPreview()
        {
            if (PreviewCanvas == null) return;
            PreviewCanvas.Children.Clear();

            double w = PreviewCanvas.Width;
            double h = PreviewCanvas.Height;
            double xRange = _geometry.XMax - _geometry.XMin;
            double yRange = _geometry.YMax - _geometry.YMin;
            if (xRange <= 0 || yRange <= 0) return;

            var cfg = ReadConfigFromSliders();

            // Pad outline.
            var padRect = new Rectangle
            {
                Width = w,
                Height = h,
                Stroke = Brushes.DarkGray,
                StrokeThickness = 2,
                Fill = Brushes.White,
                RadiusX = 10,
                RadiusY = 10,
            };
            Canvas.SetLeft(padRect, 0);
            Canvas.SetTop(padRect, 0);
            PreviewCanvas.Children.Add(padRect);

            // Edge zone bands (semi-transparent red), sized from the sliders.
            double edgeTopPx = h * (cfg.EdgePermilleTop / 1000.0);
            double edgeBottomPx = h * (cfg.EdgePermilleBottom / 1000.0);
            double edgeLeftPx = w * (cfg.EdgePermilleLeft / 1000.0);
            double edgeRightPx = w * (cfg.EdgePermilleRight / 1000.0);

            var zoneBrush = new SolidColorBrush(Color.FromArgb(60, 0xE0, 0x3B, 0x2E));
            AddZoneRect(0, 0, w, edgeTopPx, zoneBrush);                       // top
            AddZoneRect(0, h - edgeBottomPx, w, edgeBottomPx, zoneBrush);     // bottom
            AddZoneRect(0, 0, edgeLeftPx, h, zoneBrush);                     // left
            AddZoneRect(w - edgeRightPx, 0, edgeRightPx, h, zoneBrush);      // right

            AddLabel("Edge Zone", 6, 4, Brushes.Firebrick, 10);

            // Test touch position (percent of pad -> canvas px, and -> sensor units).
            double testXPct = SlTestX.Value / 100.0;
            double testYPct = SlTestY.Value / 100.0;
            double px = testXPct * w;
            double py = testYPct * h;
            double sensorX = testXPct * xRange;
            double sensorY = testYPct * yRange;

            int testMajor = (int)SlTestMajor.Value;
            int testMinor = (int)SlTestMinor.Value;
            bool isBirth = ChkIsBirth.IsChecked == true;

            var cls = PalmPreviewEngine.Classify(_geometry, cfg, testMajor, testMinor, sensorX, sensorY, isBirth);

            // Ellipse radius: Major/Minor are raw sensor units; scale them
            // against the pad's own sensor range so the ellipse is always a
            // sensible fraction of the pad regardless of hardware model.
            // This is illustrative (matches the same units the thresholds
            // above are compared against), not a literal millimeter size.
            double majorPx = Math.Max(6, (testMajor / xRange) * w);
            double minorPx = Math.Max(6, (testMinor / yRange) * h);

            Brush fingerBrush = cls switch
            {
                PalmClass.None => new SolidColorBrush(Color.FromArgb(180, 0x2E, 0xA0, 0x4A)),
                PalmClass.Local => new SolidColorBrush(Color.FromArgb(180, 0xE6, 0x9B, 0x1A)),
                PalmClass.Large => new SolidColorBrush(Color.FromArgb(180, 0xC0, 0x39, 0x2B)),
                _ => Brushes.Gray,
            };

            var ellipse = new Ellipse
            {
                Width = majorPx,
                Height = minorPx,
                Fill = fingerBrush,
                Stroke = Brushes.Black,
                StrokeThickness = 1,
            };
            Canvas.SetLeft(ellipse, px - majorPx / 2);
            Canvas.SetTop(ellipse, py - minorPx / 2);
            PreviewCanvas.Children.Add(ellipse);

            // Crosshair at the exact touch point, for precise placement.
            AddCross(px, py);

            string classLabel = cls switch
            {
                PalmClass.None => "FINGER (PALM_NONE)",
                PalmClass.Local => isBirth && PalmPreviewEngine.InEdgeZone(_geometry, cfg, sensorX, sensorY)
                    ? "PALM (PALM_LOCAL — у крайовій зоні)"
                    : "PALM (PALM_LOCAL — за формою/балами)",
                PalmClass.Large => "PALM (PALM_LARGE — весь тачпад блокується)",
                _ => "—",
            };
            ClassificationText.Text = classLabel;
            ClassificationText.Foreground = fingerBrush;
        }

        private void AddZoneRect(double x, double y, double w, double h, Brush brush)
        {
            if (w <= 0 || h <= 0) return;
            var r = new Rectangle { Width = w, Height = h, Fill = brush };
            Canvas.SetLeft(r, x);
            Canvas.SetTop(r, y);
            PreviewCanvas.Children.Add(r);
        }

        private void AddLabel(string text, double x, double y, Brush brush, double size)
        {
            var t = new TextBlock { Text = text, Foreground = brush, FontSize = size, FontWeight = FontWeights.SemiBold };
            Canvas.SetLeft(t, x);
            Canvas.SetTop(t, y);
            PreviewCanvas.Children.Add(t);
        }

        private void AddCross(double x, double y)
        {
            const double s = 8;
            var l1 = new Line { X1 = x - s, Y1 = y, X2 = x + s, Y2 = y, Stroke = Brushes.Black, StrokeThickness = 1.5 };
            var l2 = new Line { X1 = x, Y1 = y - s, X2 = x, Y2 = y + s, Stroke = Brushes.Black, StrokeThickness = 1.5 };
            PreviewCanvas.Children.Add(l1);
            PreviewCanvas.Children.Add(l2);
        }

        // Drag the test point directly on the canvas.
        private void PreviewCanvas_MouseDown(object sender, MouseButtonEventArgs e)
        {
            _draggingTestPoint = true;
            PreviewCanvas.CaptureMouse();
            UpdateTestPointFromMouse(e);
        }

        private void PreviewCanvas_MouseMove(object sender, MouseEventArgs e)
        {
            if (_draggingTestPoint) UpdateTestPointFromMouse(e);
        }

        private void PreviewCanvas_MouseUp(object sender, MouseButtonEventArgs e)
        {
            _draggingTestPoint = false;
            PreviewCanvas.ReleaseMouseCapture();
        }

        private void UpdateTestPointFromMouse(MouseEventArgs e)
        {
            var pos = e.GetPosition(PreviewCanvas);
            double xPct = Clamp(pos.X / PreviewCanvas.Width * 100.0, 0, 100);
            double yPct = Clamp(pos.Y / PreviewCanvas.Height * 100.0, 0, 100);

            _suppressEvents = true;
            try
            {
                SlTestX.Value = xPct;
                SlTestY.Value = yPct;
            }
            finally
            {
                _suppressEvents = false;
            }
            UpdateAllLabels();
            DrawPreview();
        }

        private static double Clamp(double v, double min, double max) => v < min ? min : (v > max ? max : v);

        // ---------------------------------------------------------------
        // Apply / Reset / Save / Load
        // ---------------------------------------------------------------

        private void Apply_Click(object sender, RoutedEventArgs e)
        {
            var requested = ReadConfigFromSliders();

            if (!_device.IsConnected)
            {
                SetBottomStatus("Пристрій не підключено — налаштування не збережено на драйвері (лише попередній перегляд).");
                return;
            }

            if (_device.TrySetPalmConfig(requested, out var applied))
            {
                LoadConfigIntoSliders(applied);
                DrawPreview();
                SetBottomStatus("Застосовано та збережено в реєстрі драйвера.");
            }
            else
            {
                SetBottomStatus("Не вдалося застосувати налаштування (помилка DeviceIoControl).");
            }
        }

        private void ResetDefaults_Click(object sender, RoutedEventArgs e)
        {
            if (_device.IsConnected && _device.TryResetPalmConfig(out var applied))
            {
                LoadConfigIntoSliders(applied);
                SetBottomStatus("Скинуто до значень за замовчуванням на драйвері.");
            }
            else
            {
                LoadConfigIntoSliders(PalmConfig.Default);
                SetBottomStatus(_device.IsConnected
                    ? "Не вдалося скинути на драйвері — показано локальні значення за замовчуванням."
                    : "Показано значення за замовчуванням (пристрій не підключено).");
            }
            DrawPreview();
        }

        private void SaveProfile_Click(object sender, RoutedEventArgs e)
        {
            var dlg = new SaveFileDialog
            {
                Filter = "AmtPtp palm profile (*.json)|*.json",
                FileName = "AmtPtpPalmProfile.json",
            };
            if (dlg.ShowDialog() != true) return;

            var cfg = ReadConfigFromSliders();
            var json = JsonSerializer.Serialize(cfg, new JsonSerializerOptions { WriteIndented = true });
            File.WriteAllText(dlg.FileName, json);
            SetBottomStatus($"Профіль збережено: {dlg.FileName}");
        }

        private void LoadProfile_Click(object sender, RoutedEventArgs e)
        {
            var dlg = new OpenFileDialog { Filter = "AmtPtp palm profile (*.json)|*.json" };
            if (dlg.ShowDialog() != true) return;

            try
            {
                var json = File.ReadAllText(dlg.FileName);
                var cfg = JsonSerializer.Deserialize<PalmConfig>(json);
                LoadConfigIntoSliders(cfg.Clamped());
                DrawPreview();
                SetBottomStatus($"Профіль завантажено: {dlg.FileName}. Натисніть «Застосувати», щоб зберегти на драйвері.");
            }
            catch (Exception ex)
            {
                SetBottomStatus($"Помилка завантаження профілю: {ex.Message}");
            }
        }

        private void SetBottomStatus(string text) => BottomStatusText.Text = text;

        protected override void OnClosed(EventArgs e)
        {
            _device.Dispose();
            base.OnClosed(e);
        }
    }
}