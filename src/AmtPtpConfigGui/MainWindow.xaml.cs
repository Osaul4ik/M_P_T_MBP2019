using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Shapes;
using System.Windows.Threading;
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

        private readonly DispatcherTimer _liveTimer;
        private bool _liveEnabled;
        private uint _lastLiveSequence;

        // Per-contact EMA smoothing state for the live overlay's ellipse
        // size only (Major/Minor as *rendered*, never touches the raw
        // values used for palm classification thresholds elsewhere).
        //
        // Root cause of visible height jitter on a static, fully-rested
        // touch: Major/Minor are the sensor firmware's own touch-ellipse
        // long/short axis fit (touch_major/touch_minor), not literally
        // "extent along the pad's X axis" / "extent along the pad's Y
        // axis" - there's no orientation angle in the wire format. The
        // long axis (mapped here to width) is well-conditioned and stable
        // frame-to-frame; the short axis (mapped to height) is a much
        // smaller residual in the firmware's ellipse fit and is inherently
        // noisier per-frame, even for a perfectly still finger - that's
        // why width looked rock solid while height wobbled a little.
        // Smoothing only the display, not the classifier input, keeps
        // palm-rejection tuning honest while fixing the visual wobble.
        private readonly Dictionary<uint, (double Major, double Minor)> _liveGeometrySmooth = new();
        private const double LiveGeometrySmoothAlpha = 0.25; // lower = smoother/slower to react

        private sealed class CornerExtrema
        {
            public int Samples;

            public short MinRawX = short.MaxValue;
            public short MaxRawX = short.MinValue;
            public short MinRawY = short.MaxValue;
            public short MaxRawY = short.MinValue;

            public ushort MinNormX = ushort.MaxValue;
            public ushort MaxNormX = 0;
            public ushort MinNormY = ushort.MaxValue;
            public ushort MaxNormY = 0;

            public void Update(short rawX, short rawY, ushort normX, ushort normY)
            {
                Samples++;

                if (rawX < MinRawX) MinRawX = rawX;
                if (rawX > MaxRawX) MaxRawX = rawX;
                if (rawY < MinRawY) MinRawY = rawY;
                if (rawY > MaxRawY) MaxRawY = rawY;

                if (normX < MinNormX) MinNormX = normX;
                if (normX > MaxNormX) MaxNormX = normX;
                if (normY < MinNormY) MinNormY = normY;
                if (normY > MaxNormY) MaxNormY = normY;
            }

            public string ToText()
            {
                if (Samples == 0)
                    return "samples=0";

                return
                    $"samples={Samples}; " +
                    $"RawX=[{MinRawX}..{MaxRawX}] " +
                    $"RawY=[{MinRawY}..{MaxRawY}] " +
                    $"NormX=[{MinNormX}..{MaxNormX}] " +
                    $"NormY=[{MinNormY}..{MaxNormY}]";
            }
        }

        private readonly CornerExtrema _topLeft = new();
        private readonly CornerExtrema _topRight = new();
        private readonly CornerExtrema _bottomLeft = new();
        private readonly CornerExtrema _bottomRight = new();
        private int _liveCornerSamples;

        public MainWindow()
        {
            InitializeComponent();
            _uiReady = true;

            _liveTimer = new DispatcherTimer
            {
                Interval = TimeSpan.FromMilliseconds(33)
            };
            _liveTimer.Tick += LiveTimer_Tick;

            Loaded += (_, _) => Reconnect();
            Closed += (_, _) =>
            {
                if (_liveEnabled)
                    _device.SetLiveEnabled(false);
                _liveTimer.Stop();
                _device.Dispose();
            };
        }

        // ---------------------------------------------------------------
        // Connection handling
        // ---------------------------------------------------------------

        private void Reconnect_Click(object sender, RoutedEventArgs e) => Reconnect();

        private void SaveErrors_Click(object sender, RoutedEventArgs e)
        {
            if (_diagnosticLog.Count == 0 && _liveCornerSamples == 0)
            {
                MessageBox.Show(
                    "Ще немає діагностичних даних або Live-калібровки.",
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
                var lines = new List<string>(_diagnosticLog);

                lines.Add("");
                lines.Add("===== Live Touch Calibration / Corner Extrema =====");
                lines.Add($"Timestamp: {DateTime.Now:yyyy-MM-dd HH:mm:ss}");
                lines.Add($"Live samples total: {_liveCornerSamples}");
                lines.Add("");
                lines.Add($"TOP-LEFT     {_topLeft.ToText()}");
                lines.Add($"TOP-RIGHT    {_topRight.ToText()}");
                lines.Add($"BOTTOM-LEFT  {_bottomLeft.ToText()}");
                lines.Add($"BOTTOM-RIGHT {_bottomRight.ToText()}");

                File.WriteAllLines(dialog.FileName, lines);
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
            if (_liveEnabled)
            {
                _device.SetLiveEnabled(false);
                _liveEnabled = false;
                _liveTimer.Stop();
                if (ChkLive != null)
                    ChkLive.IsChecked = false;
            }

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
        // Live touch monitor
        // ---------------------------------------------------------------

        private void ResetCornerExtrema()
        {
            ResetCorner(_topLeft);
            ResetCorner(_topRight);
            ResetCorner(_bottomLeft);
            ResetCorner(_bottomRight);
            _liveCornerSamples = 0;
        }

        private static void ResetCorner(CornerExtrema c)
        {
            c.Samples = 0;
            c.MinRawX = short.MaxValue;
            c.MaxRawX = short.MinValue;
            c.MinRawY = short.MaxValue;
            c.MaxRawY = short.MinValue;
            c.MinNormX = ushort.MaxValue;
            c.MaxNormX = 0;
            c.MinNormY = ushort.MaxValue;
            c.MaxNormY = 0;
        }

        private static CornerExtrema GetCorner(
            CornerExtrema tl,
            CornerExtrema tr,
            CornerExtrema bl,
            CornerExtrema br,
            ushort normX,
            ushort normY,
            double xRange,
            double yRange)
        {
            double px = xRange > 0 ? normX / xRange : 0.0;
            double py = yRange > 0 ? normY / yRange : 0.0;

            // Divide the pad into four quadrants. Each sample belongs to exactly
            // one corner; this is intentionally simple for calibration passes.
            if (py < 0.5)
                return px < 0.5 ? tl : tr;
            else
                return px < 0.5 ? bl : br;
        }

        private void AccumulateCornerExtrema(LiveFrame frame)
        {
            if (frame.Contacts == null || frame.ContactCount == 0)
                return;

            double xRange = _geometry.XMax - _geometry.XMin;
            double yRange = _geometry.YMax - _geometry.YMin;

            if (xRange <= 0 || yRange <= 0)
                return;

            for (int i = 0; i < frame.ContactCount && i < frame.Contacts.Length; i++)
            {
                var c = frame.Contacts[i];
                var target = GetCorner(
                    _topLeft,
                    _topRight,
                    _bottomLeft,
                    _bottomRight,
                    c.X,
                    c.Y,
                    xRange,
                    yRange);

                target.Update(c.RawX, c.RawY, c.X, c.Y);
                _liveCornerSamples++;
            }
        }

        private void Live_Changed(object sender, RoutedEventArgs e)
        {
            if (!_uiReady)
                return;

            bool enabled = ChkLive.IsChecked == true;

            if (!_device.IsConnected)
            {
                _liveEnabled = false;
                _liveTimer.Stop();
                LiveStatusText.Text = "Live: пристрій не підключено";
                return;
            }

            if (!_device.SetLiveEnabled(enabled))
            {
                _liveEnabled = false;
                _liveTimer.Stop();
                if (ChkLive.IsChecked == true)
                    ChkLive.IsChecked = false;
                LiveStatusText.Text = "Live: помилка";
                LiveCoordText.Text = "Live: координати —";
                LiveCornerText.Text = "Кути: TL 0 | TR 0 | BL 0 | BR 0";
                return;
            }

            _liveEnabled = enabled;
            _lastLiveSequence = 0;
            _liveGeometrySmooth.Clear();

            if (enabled)
            {
                ResetCornerExtrema();
                _liveTimer.Start();
                LiveStatusText.Text = "Live: очікування… | кути: збираються";
            }
            else
            {
                _liveTimer.Stop();
                LiveStatusText.Text = "Live: вимкнено";
                LiveCoordText.Text = "Live: координати —";
                LiveCornerText.Text = "Кути: TL 0 | TR 0 | BL 0 | BR 0";
                DrawPreview();
            }
        }

        private void LiveTimer_Tick(object? sender, EventArgs e)
        {
            if (!_liveEnabled || !_device.IsConnected)
                return;

            if (!_device.TryGetLiveFrame(out var frame))
                return;

            if (frame.Sequence == 0 || frame.Sequence == _lastLiveSequence)
                return;

            _lastLiveSequence = frame.Sequence;
            AccumulateCornerExtrema(frame);
            DrawLiveOverlay(frame);

            if (frame.ContactCount > 0 && frame.Contacts != null)
            {
                var coordLines = new System.Collections.Generic.List<string>();

                for (int i = 0; i < frame.ContactCount && i < frame.Contacts.Length; i++)
                {
                    var c = frame.Contacts[i];

                    coordLines.Add(
                        $"C{i} ID {c.ContactID}: " +
                        $"Raw X={c.RawX}, Y={c.RawY}  |  " +
                        $"Norm X={c.X}, Y={c.Y}");
                }

                LiveCoordText.Text = string.Join("   •   ", coordLines);
            }
            else
            {
                LiveCoordText.Text = "Live: координати — немає активних контактів";
            }

            LiveCornerText.Text =
                $"Кути: TL {_topLeft.Samples} | TR {_topRight.Samples} | " +
                $"BL {_bottomLeft.Samples} | BR {_bottomRight.Samples}";

            LiveStatusText.Text =
                $"Live: {frame.ContactCount} дот. | seq {frame.Sequence}" +
                (frame.ButtonDown != 0 ? " | BUTTON" : "") +
                (frame.LargePalmBlanked != 0 ? " | PALM" : "");
        }

        private void DrawLiveOverlay(LiveFrame frame)
        {
            DrawPreview();

            if (!_liveEnabled || PreviewCanvas == null || frame.Contacts == null)
                return;

            double w = PreviewCanvas.Width;
            double h = PreviewCanvas.Height;
            double xRange = _geometry.XMax - _geometry.XMin;
            double yRange = _geometry.YMax - _geometry.YMin;

            if (xRange <= 0 || yRange <= 0)
                return;

            // Track which contact IDs are still alive this frame so stale
            // smoothing state for lifted contacts doesn't linger forever
            // (and doesn't get reused if a ContactID happens to be recycled
            // much later).
            var seenIds = new HashSet<uint>();

            for (int i = 0; i < frame.ContactCount && i < frame.Contacts.Length; i++)
            {
                var c = frame.Contacts[i];
                seenIds.Add(c.ContactID);

                // LiveFrame X/Y are already normalized to the device's
                // coordinate origin by the driver:
                //   X = rawX - XMin
                //   Y = YMax - rawY
                // Therefore do NOT subtract _geometry.XMin/YMin again.
                double px = c.X / xRange * w;
                double py = c.Y / yRange * h;

                px = Math.Clamp(px, 0, w);
                py = Math.Clamp(py, 0, h);

                bool isPalm = c.PalmSuspect != 0;

                // Outline/label color still reflects lifecycle phase so you
                // can see DOWN/MOVE/UP at a glance; a palm-suspect contact
                // always renders with a red fill regardless of phase, so it
                // reads unambiguously even at a glance.
                Brush outline = isPalm
                    ? Brushes.Firebrick
                    : c.Phase == 1
                        ? Brushes.LimeGreen
                        : c.Phase == 3
                            ? Brushes.Orange
                            : Brushes.DeepSkyBlue;

                Brush fill = isPalm
                    ? new SolidColorBrush(Color.FromArgb(150, 0xE8, 0x11, 0x23))
                    : new SolidColorBrush(Color.FromArgb(90,
                        ((SolidColorBrush)outline).Color.R,
                        ((SolidColorBrush)outline).Color.G,
                        ((SolidColorBrush)outline).Color.B));

                // Contact geometry: Major/Minor are raw sensor units from the
                // nearest matched raw frame (0 for a reconstructed UP contact
                // with no raw match this frame). Scale them against the pad's
                // own sensor range - same convention as the offline test
                // preview ellipse in DrawPreview() - so real touches and the
                // manual test touch are visually comparable.
                //
                // Light per-contact EMA smoothing on the DISPLAYED size only
                // (see _liveGeometrySmooth comment) - the short/minor axis of
                // the firmware's ellipse fit is noticeably noisier per-frame
                // than the long/major axis even for a perfectly still touch,
                // which otherwise shows up as visible jitter in the rendered
                // height while the width looks rock solid. A fresh DOWN
                // snaps immediately to the raw size instead of fading in.
                double rawMajor = c.Major;
                double rawMinor = c.Minor;
                double dispMajor, dispMinor;

                if (c.Phase == 1 /* DOWN */ || !_liveGeometrySmooth.TryGetValue(c.ContactID, out var prevGeom))
                {
                    dispMajor = rawMajor;
                    dispMinor = rawMinor;
                }
                else
                {
                    dispMajor = prevGeom.Major + (rawMajor - prevGeom.Major) * LiveGeometrySmoothAlpha;
                    dispMinor = prevGeom.Minor + (rawMinor - prevGeom.Minor) * LiveGeometrySmoothAlpha;
                }
                _liveGeometrySmooth[c.ContactID] = (dispMajor, dispMinor);

                double majorPx = dispMajor > 0 ? Math.Max(10, dispMajor / xRange * w) : 26;
                double minorPx = dispMinor > 0 ? Math.Max(10, dispMinor / yRange * h) : 26;

                var footprint = new Ellipse
                {
                    Width = majorPx,
                    Height = minorPx,
                    Fill = fill,
                    Stroke = outline,
                    StrokeThickness = isPalm ? 2.5 : 2
                };
                Canvas.SetLeft(footprint, px - majorPx / 2);
                Canvas.SetTop(footprint, py - minorPx / 2);
                PreviewCanvas.Children.Add(footprint);

                var center = new Ellipse
                {
                    Width = 6,
                    Height = 6,
                    Fill = outline
                };
                Canvas.SetLeft(center, px - 3);
                Canvas.SetTop(center, py - 3);
                PreviewCanvas.Children.Add(center);

                string tag = isPalm ? $"ID {c.ContactID} · PALM" : $"ID {c.ContactID}";
                var label = new TextBlock
                {
                    Text = tag,
                    FontSize = 11,
                    FontWeight = FontWeights.SemiBold,
                    Foreground = isPalm ? Brushes.White : outline,
                    Background = isPalm ? Brushes.Firebrick : Brushes.White,
                    Padding = new Thickness(4, 1, 4, 1)
                };
                Canvas.SetLeft(label, px + Math.Max(18, majorPx / 2 + 4));
                Canvas.SetTop(label, py - 9);
                PreviewCanvas.Children.Add(label);
            }

            // Drop smoothing state for any contact that isn't in this frame
            // anymore (lifted, or dropped) so it doesn't linger indefinitely
            // and doesn't leak into a later, unrelated contact that happens
            // to reuse the same ContactID far in the future.
            if (_liveGeometrySmooth.Count > 0)
            {
                var stale = new List<uint>();
                foreach (var id in _liveGeometrySmooth.Keys)
                {
                    if (!seenIds.Contains(id))
                        stale.Add(id);
                }
                foreach (var id in stale)
                    _liveGeometrySmooth.Remove(id);
            }
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