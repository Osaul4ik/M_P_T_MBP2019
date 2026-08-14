using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Shapes;
using System.Windows.Threading;
using AmtPtpConfigGui.Native;
using Microsoft.Win32;

namespace AmtPtpConfigGui
{
    public partial class MainWindow : Window
    {
        private static SolidColorBrush Frozen(byte a, byte r, byte g, byte b)
        {
            var brush = new SolidColorBrush(Color.FromArgb(a, r, g, b));
            brush.Freeze();
            return brush;
        }

        private static readonly SolidColorBrush ConnectedBrush = Frozen(0xFF, 0x1D, 0x9A, 0x6C);
        private static readonly SolidColorBrush DisconnectedBrush = Frozen(0xFF, 0xE5, 0x48, 0x4D);
        private static readonly SolidColorBrush LiveDotIdleBrush = Frozen(0xFF, 0x9A, 0xA1, 0xAC);
        private static readonly LinearGradientBrush GlassPadBrush = CreateGlassPadBrush();

        private static LinearGradientBrush CreateGlassPadBrush()
        {
            // Brushed-aluminum silver, like the MacBook's own trackpad glass
            // set into its aluminum body, instead of the previous dark
            // "glass device" look.
            var brush = new LinearGradientBrush
            {
                StartPoint = new Point(0, 0),
                EndPoint = new Point(1, 1),
            };
            brush.GradientStops.Add(new GradientStop(Color.FromRgb(0xF3, 0xF3, 0xF4), 0.0));
            brush.GradientStops.Add(new GradientStop(Color.FromRgb(0xE2, 0xE3, 0xE5), 0.55));
            brush.GradientStops.Add(new GradientStop(Color.FromRgb(0xD3, 0xD4, 0xD7), 1.0));
            brush.Freeze();
            return brush;
        }

        private static readonly SolidColorBrush GlassPadStrokeBrush = Frozen(0xFF, 0xB7, 0xB9, 0xBD);
        private static readonly SolidColorBrush CrosshairBrush = Frozen(0x50, 0x00, 0x00, 0x00);
        private static readonly SolidColorBrush EdgeZoneBrush = Frozen(60, 0xFF, 0x6D, 0x2E);
        private static readonly SolidColorBrush EdgeZoneLabelBrush = Frozen(0xFF, 0xC4, 0x45, 0x0B);

        // Test-touch classification fill - the same semantic accepted/
        // soft-reject/hard-reject colors, tuned to stay legible (with a
        // dark stroke, see DrawPreview/AddCross) on the silver pad surface.
        private static readonly SolidColorBrush PalmNoneBrush = Frozen(225, 0x1D, 0x9A, 0x6C);
        private static readonly SolidColorBrush PalmLocalBrush = Frozen(225, 0xE0, 0x9A, 0x0A);
        private static readonly SolidColorBrush PalmLargeBrush = Frozen(225, 0xE5, 0x48, 0x4D);

        // Live-overlay outline colors reuse WPF's own static Brushes.* -
        // those are already immutable singletons, no allocation there -
        // except the palm-suspect outline, which is unified with
        // LiveFillPalm's exact hue below instead of the mismatched
        // built-in Brushes.Firebrick previously used only for the outline.
        private static readonly SolidColorBrush LivePalmOutline = Frozen(0xFF, 0xE8, 0x11, 0x23);
        private static readonly SolidColorBrush LiveFillDown = Frozen(90, Colors.LimeGreen.R, Colors.LimeGreen.G, Colors.LimeGreen.B);
        private static readonly SolidColorBrush LiveFillUp = Frozen(90, Colors.Orange.R, Colors.Orange.G, Colors.Orange.B);
        private static readonly SolidColorBrush LiveFillMove = Frozen(90, Colors.DeepSkyBlue.R, Colors.DeepSkyBlue.G, Colors.DeepSkyBlue.B);
        private static readonly SolidColorBrush LiveFillPalm = Frozen(150, 0xE8, 0x11, 0x23);
        private static readonly SolidColorBrush LiveTagBgBrush = Frozen(230, 0x14, 0x16, 0x1B);

        private readonly DeviceIo _device = new DeviceIo();
        private readonly List<string> _diagnosticLog = new();
        private PadGeometry _geometry = PadGeometry.Fallback;
        private bool _geometryFromDevice;
        private bool _suppressEvents;
        private bool _draggingTestPoint;
        private bool _uiReady;

        private readonly DispatcherTimer _liveTimer;
        private bool _liveEnabled;
        private uint _lastLiveSequence;

        // Pooled live-overlay shapes (one triple per AMT_LIVE_MAX_CONTACTS
        // slot), created once and reused every tick - see DrawLiveOverlay.
        // Reusing these instead of allocating fresh Ellipse/TextBlock
        // objects and clearing/rebuilding the whole canvas on every frame
        // is what keeps Live mode from pegging the CPU: WPF layout/render
        // cost comes from visual-tree churn, not from the IOCTL poll itself.
        private const int LiveOverlaySlots = 5; // matches AMT_LIVE_MAX_CONTACTS in Public.h
        private readonly Ellipse[] _liveFootprints = new Ellipse[LiveOverlaySlots];
        private readonly Ellipse[] _liveCenters = new Ellipse[LiveOverlaySlots];
        private readonly TextBlock[] _liveLabels = new TextBlock[LiveOverlaySlots];
        private bool _liveOverlayInitialized;

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
                // 20 Hz instead of the previous 30 Hz - still visually
                // smooth for a debug/calibration overlay, ~35% fewer
                // ticks (and therefore fewer IOCTL polls + overlay
                // updates) than before.
                Interval = TimeSpan.FromMilliseconds(50)
            };
            _liveTimer.Tick += LiveTimer_Tick;

            Loaded += (_, _) => Reconnect();
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
                HideAllLiveOverlayElements();
            }

            bool connected = _device.TryConnect();

            if (connected)
            {
                StatusDot.Fill = ConnectedBrush;
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

                if (_device.TryGetPointerConfig(out var pointerCfg))
                    LoadPointerConfigIntoControls(pointerCfg);
                else
                    LoadPointerConfigIntoControls(PointerConfig.Default);

                if (_device.TryGetScrollConfig(out var scrollCfg))
                    LoadScrollConfigIntoControls(scrollCfg);
                else
                    LoadScrollConfigIntoControls(ScrollConfig.Default);

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
                StatusDot.Fill = DisconnectedBrush;
                StatusText.Text = "Пристрій не знайдено — режим попереднього перегляду";
                LoadConfigIntoSliders(PalmConfig.Default);
                LoadPointerConfigIntoControls(PointerConfig.Default);
                LoadScrollConfigIntoControls(ScrollConfig.Default);
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
                SetLiveDot(active: false);
                HideAllLiveOverlayElements();
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
                SetLiveDot(active: null); // error - solid red, no pulse
                HideAllLiveOverlayElements();
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
                SetLiveDot(active: true);
            }
            else
            {
                _liveTimer.Stop();
                LiveStatusText.Text = "Live: вимкнено";
                LiveCoordText.Text = "Live: координати —";
                LiveCornerText.Text = "Кути: TL 0 | TR 0 | BL 0 | BR 0";
                SetLiveDot(active: false);
                HideAllLiveOverlayElements();
                DrawPreview();
            }
        }

        // Drives the small dot next to "Live: ..." in the header toolbar:
        // a soft pulsing green while frames are actively streaming, a
        // steady muted gray when idle, and a steady red (no pulse - a
        // pulse would suggest "working", which an error state isn't) if
        // enabling live mode failed.
        private void SetLiveDot(bool? active)
        {
            if (LiveDot == null) return;

            var pulse = (Storyboard)FindResource("LivePulseStoryboard");
            pulse.Stop(this);
            LiveDot.Opacity = 1.0;

            switch (active)
            {
                case true:
                    LiveDot.Fill = ConnectedBrush;
                    pulse.Begin(this, isControllable: true);
                    break;
                case false:
                    LiveDot.Fill = LiveDotIdleBrush;
                    break;
                case null:
                    LiveDot.Fill = DisconnectedBrush;
                    break;
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
                var coordLines = new List<string>();

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

        // Creates the pooled overlay shapes once (lazy - PreviewCanvas/
        // LiveOverlayCanvas aren't valid until the window is loaded) and
        // adds them to LiveOverlayCanvas a single time. Every subsequent
        // live tick only mutates their properties - no further
        // Add/Remove/Clear on the canvas, which is what used to force a
        // full WPF layout+render pass on every frame.
        private void EnsureLiveOverlayElements()
        {
            if (_liveOverlayInitialized || LiveOverlayCanvas == null)
                return;

            for (int i = 0; i < LiveOverlaySlots; i++)
            {
                var footprint = new Ellipse { Visibility = Visibility.Collapsed };
                var center = new Ellipse { Width = 6, Height = 6, Visibility = Visibility.Collapsed };
                var label = new TextBlock
                {
                    FontSize = 11,
                    FontWeight = FontWeights.SemiBold,
                    Padding = new Thickness(5, 2, 5, 2),
                    Visibility = Visibility.Collapsed,
                };

                LiveOverlayCanvas.Children.Add(footprint);
                LiveOverlayCanvas.Children.Add(center);
                LiveOverlayCanvas.Children.Add(label);

                _liveFootprints[i] = footprint;
                _liveCenters[i] = center;
                _liveLabels[i] = label;
            }

            _liveOverlayInitialized = true;
        }

        // Hides (doesn't remove - removing/re-adding would defeat the
        // whole point of pooling) every pooled overlay shape. Called when
        // Live mode turns off or a frame has nothing to show, so stale
        // footprints from the last live touch don't linger on screen.
        private void HideAllLiveOverlayElements()
        {
            if (!_liveOverlayInitialized)
                return;

            for (int i = 0; i < LiveOverlaySlots; i++)
            {
                _liveFootprints[i].Visibility = Visibility.Collapsed;
                _liveCenters[i].Visibility = Visibility.Collapsed;
                _liveLabels[i].Visibility = Visibility.Collapsed;
            }
        }

        private void DrawLiveOverlay(LiveFrame frame)
        {
            if (LiveOverlayCanvas == null)
                return;

            if (!_liveEnabled || frame.Contacts == null)
            {
                HideAllLiveOverlayElements();
                return;
            }

            double w = LiveOverlayCanvas.Width;
            double h = LiveOverlayCanvas.Height;
            double xRange = _geometry.XMax - _geometry.XMin;
            double yRange = _geometry.YMax - _geometry.YMin;

            if (xRange <= 0 || yRange <= 0)
            {
                HideAllLiveOverlayElements();
                return;
            }

            EnsureLiveOverlayElements();

            // Track which contact IDs are still alive this frame so stale
            // smoothing state for lifted contacts doesn't linger forever
            // (and doesn't get reused if a ContactID happens to be recycled
            // much later).
            var seenIds = new HashSet<uint>();

            int slots = Math.Min(frame.ContactCount, Math.Min(frame.Contacts.Length, LiveOverlaySlots));

            for (int i = 0; i < slots; i++)
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
                Brush outline;
                Brush fill;

                if (isPalm)
                {
                    outline = LivePalmOutline;
                    fill = LiveFillPalm;
                }
                else if (c.Phase == 1)
                {
                    outline = Brushes.LimeGreen;
                    fill = LiveFillDown;
                }
                else if (c.Phase == 3)
                {
                    outline = Brushes.Orange;
                    fill = LiveFillUp;
                }
                else
                {
                    outline = Brushes.DeepSkyBlue;
                    fill = LiveFillMove;
                }

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

                var footprint = _liveFootprints[i];
                footprint.Width = majorPx;
                footprint.Height = minorPx;
                footprint.Fill = fill;
                footprint.Stroke = outline;
                footprint.StrokeThickness = isPalm ? 2.5 : 2;
                Canvas.SetLeft(footprint, px - majorPx / 2);
                Canvas.SetTop(footprint, py - minorPx / 2);
                footprint.Visibility = Visibility.Visible;

                var center = _liveCenters[i];
                center.Fill = outline;
                Canvas.SetLeft(center, px - 3);
                Canvas.SetTop(center, py - 3);
                center.Visibility = Visibility.Visible;

                string tag = isPalm ? $"ID {c.ContactID} · PALM" : $"ID {c.ContactID}";
                var label = _liveLabels[i];
                label.Text = tag;
                label.Foreground = isPalm ? Brushes.White : outline;
                label.Background = isPalm ? LivePalmOutline : LiveTagBgBrush;
                Canvas.SetLeft(label, px + Math.Max(18, majorPx / 2 + 4));
                Canvas.SetTop(label, py - 9);
                label.Visibility = Visibility.Visible;
            }

            // Any pooled slots beyond this frame's contact count belong to
            // contacts that just lifted (or never existed) - hide them
            // rather than leaving the last-drawn footprint stuck on screen.
            for (int i = slots; i < LiveOverlaySlots; i++)
            {
                _liveFootprints[i].Visibility = Visibility.Collapsed;
                _liveCenters[i].Visibility = Visibility.Collapsed;
                _liveLabels[i].Visibility = Visibility.Collapsed;
            }

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

        // ---------------------------------------------------------------
        // Pointer tab <-> PointerConfig plumbing
        private bool _suppressPointerEvents;

        private void LoadPointerConfigIntoControls(PointerConfig cfg)
        {
            _suppressPointerEvents = true;
            try
            {
                cfg = cfg.Clamped();
                SlForceTapThreshold.Value = cfg.ForceTapThreshold;
                SlCursorSmoothing.Value = cfg.CursorSmoothingPercent;
                SlCursorSpeed.Value = cfg.CursorSpeedPercent;
                SlCursorDeadzone.Value = cfg.CursorDeadzone;
                SlCursorSlowVelocity.Value = cfg.CursorSlowVelocity;
                SlCursorFastVelocity.Value = cfg.CursorFastVelocity;

                RadioButton selected = cfg.ForceTapAction switch
                {
                    PointerConfig.ActionMiddleClick => RbActionMiddleClick,
                    PointerConfig.ActionDoubleClick => RbActionDoubleClick,
                    _ => RbActionContextMenu,
                };
                RbActionContextMenu.IsChecked = ReferenceEquals(selected, RbActionContextMenu);
                RbActionMiddleClick.IsChecked = ReferenceEquals(selected, RbActionMiddleClick);
                RbActionDoubleClick.IsChecked = ReferenceEquals(selected, RbActionDoubleClick);
            }
            finally { _suppressPointerEvents = false; }
            UpdatePointerLabels();
        }

        private PointerConfig ReadPointerConfigFromControls()
        {
            var c = PointerConfig.Default;
            c.ForceTapThreshold = (uint)SlForceTapThreshold.Value;
            c.CursorSmoothingPercent = (uint)SlCursorSmoothing.Value;
            c.CursorSpeedPercent = (uint)SlCursorSpeed.Value;
            c.CursorDeadzone = (uint)SlCursorDeadzone.Value;
            c.CursorSlowVelocity = (uint)SlCursorSlowVelocity.Value;
            c.CursorFastVelocity = (uint)SlCursorFastVelocity.Value;
            c.ForceTapAction =
                RbActionMiddleClick.IsChecked == true ? PointerConfig.ActionMiddleClick :
                RbActionDoubleClick.IsChecked == true ? PointerConfig.ActionDoubleClick :
                PointerConfig.ActionContextMenu;
            return c.Clamped();
        }

        private void UpdatePointerLabels()
        {
            if (LblForceTapThreshold == null) return;
            LblForceTapThreshold.Text = $"{SlForceTapThreshold.Value:0}";
            LblCursorSmoothing.Text = $"{SlCursorSmoothing.Value:0}%";
            LblCursorSpeed.Text = $"{SlCursorSpeed.Value:0}%";
            LblCursorDeadzone.Text = $"{SlCursorDeadzone.Value:0}";
            LblCursorSlowVelocity.Text = $"{SlCursorSlowVelocity.Value:0}";
            LblCursorFastVelocity.Text = $"{SlCursorFastVelocity.Value:0}";
        }

        private void PointerSlider_Changed(object sender, RoutedEventArgs e)
        {
            if (!_uiReady || _suppressPointerEvents) return;
            UpdatePointerLabels();
        }

        private void PointerAction_Changed(object sender, RoutedEventArgs e)
        {
            if (!_uiReady || _suppressPointerEvents) return;
        }

        // Scroll tab <-> ScrollConfig plumbing
        private bool _suppressScrollEvents;

        private void LoadScrollConfigIntoControls(ScrollConfig cfg)
        {
            _suppressScrollEvents = true;
            try
            {
                cfg = cfg.Clamped();
                SlScrollSpeed.Value = cfg.SpeedPercent;
                SlScrollFastSpeed.Value = cfg.FastSpeedPercent;
                SlScrollSmoothing.Value = cfg.SmoothingPercent;
                SlScrollDeadzone.Value = cfg.Deadzone;
                SlScrollFastVelocity.Value = cfg.FastVelocity;
            }
            finally { _suppressScrollEvents = false; }
            UpdateScrollLabels();
        }

        private ScrollConfig ReadScrollConfigFromControls()
        {
            var c = ScrollConfig.Default;
            c.SpeedPercent = (uint)SlScrollSpeed.Value;
            c.FastSpeedPercent = (uint)SlScrollFastSpeed.Value;
            c.SmoothingPercent = (uint)SlScrollSmoothing.Value;
            c.Deadzone = (uint)SlScrollDeadzone.Value;
            c.FastVelocity = (uint)SlScrollFastVelocity.Value;
            return c.Clamped();
        }

        private void UpdateScrollLabels()
        {
            if (LblScrollSpeed == null) return;
            LblScrollSpeed.Text = $"{SlScrollSpeed.Value:0}%";
            LblScrollFastSpeed.Text = $"{SlScrollFastSpeed.Value:0}%";
            LblScrollSmoothing.Text = $"{SlScrollSmoothing.Value:0}%";
            LblScrollDeadzone.Text = $"{SlScrollDeadzone.Value:0}";
            LblScrollFastVelocity.Text = $"{SlScrollFastVelocity.Value:0}";
        }

        private void ScrollSlider_Changed(object sender, RoutedEventArgs e)
        {
            if (!_uiReady || _suppressScrollEvents) return;
            UpdateScrollLabels();
        }

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

            // Pad surface, drawn as a dark glass panel (see GlassPadBrush)
            // instead of a plain white rectangle, so it reads as an actual
            // trackpad rather than a generic canvas.
            var padRect = new Rectangle
            {
                Width = w,
                Height = h,
                Stroke = GlassPadStrokeBrush,
                StrokeThickness = 1.5,
                Fill = GlassPadBrush,
                RadiusX = 14,
                RadiusY = 14,
            };
            Canvas.SetLeft(padRect, 0);
            Canvas.SetTop(padRect, 0);
            PreviewCanvas.Children.Add(padRect);

            // Faint center crosshair guides, purely decorative/orientation -
            // helps eyeball where the middle of the pad is at a glance,
            // the way a real calibration jig would mark it.
            AddGuideLine(w / 2, 0, w / 2, h);
            AddGuideLine(0, h / 2, w, h / 2);

            // Edge zone bands (semi-transparent red), sized from the sliders.
            double edgeTopPx = h * (cfg.EdgePermilleTop / 1000.0);
            double edgeBottomPx = h * (cfg.EdgePermilleBottom / 1000.0);
            double edgeLeftPx = w * (cfg.EdgePermilleLeft / 1000.0);
            double edgeRightPx = w * (cfg.EdgePermilleRight / 1000.0);

            AddZoneRect(0, 0, w, edgeTopPx, EdgeZoneBrush);                       // top
            AddZoneRect(0, h - edgeBottomPx, w, edgeBottomPx, EdgeZoneBrush);     // bottom
            AddZoneRect(0, 0, edgeLeftPx, h, EdgeZoneBrush);                     // left
            AddZoneRect(w - edgeRightPx, 0, edgeRightPx, h, EdgeZoneBrush);      // right

            AddLabel("EDGE ZONE", 8, 6, EdgeZoneLabelBrush, 10);

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
                PalmClass.None => PalmNoneBrush,
                PalmClass.Local => PalmLocalBrush,
                PalmClass.Large => PalmLargeBrush,
                _ => Brushes.Gray,
            };

            var ellipse = new Ellipse
            {
                Width = majorPx,
                Height = minorPx,
                Fill = fingerBrush,
                Stroke = Brushes.White,
                StrokeThickness = 1.5,
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
            var t = new TextBlock
            {
                Text = text,
                Foreground = brush,
                FontSize = size,
                FontWeight = FontWeights.Bold,
                // Letter-spacing-ish caption treatment (small caps feel) for
                // the "EDGE ZONE" tag - a technical/instrument detail rather
                // than a plain inline label competing with the readout text.
                FontFamily = new FontFamily("Segoe UI Semibold, Segoe UI"),
            };
            Canvas.SetLeft(t, x);
            Canvas.SetTop(t, y);
            PreviewCanvas.Children.Add(t);
        }

        // Faint, thin guide line across the pad glass - purely a visual
        // calibration aid (center crosshair), not tied to any threshold.
        private void AddGuideLine(double x1, double y1, double x2, double y2)
        {
            var line = new Line
            {
                X1 = x1,
                Y1 = y1,
                X2 = x2,
                Y2 = y2,
                Stroke = CrosshairBrush,
                StrokeThickness = 1,
            };
            PreviewCanvas.Children.Add(line);
        }

        private void AddCross(double x, double y)
        {
            const double s = 8;
            var l1 = new Line { X1 = x - s, Y1 = y, X2 = x + s, Y2 = y, Stroke = Brushes.Black, StrokeThickness = 1.5, Opacity = 0.55 };
            var l2 = new Line { X1 = x, Y1 = y - s, X2 = x, Y2 = y + s, Stroke = Brushes.Black, StrokeThickness = 1.5, Opacity = 0.55 };
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

        // NOTE: this is the GLOBAL bottom-bar Apply/Reset - it is always
        // visible regardless of which tab (Palm/Scroll/Pointer) is
        // currently selected. It must commit BOTH PalmConfig and
        // PointerConfig, or a user who edits the Pointer tab and hits this
        // button (rather than the Pointer tab's own local Apply button)
        // would see their Force Tap changes silently discarded - the
        // sliders would keep showing the new values locally, but nothing
        // would actually reach the driver/registry, so they'd revert to
        // the old/default values the next time the GUI reconnects.
        private void Save_Click(object sender, RoutedEventArgs e)
        {
            var requestedPalm = ReadConfigFromSliders();
            var requestedPointer = ReadPointerConfigFromControls();
            var requestedScroll = ReadScrollConfigFromControls();

            if (!_device.IsConnected)
            {
                SetBottomStatus("Пристрій не підключено — налаштування не збережено на драйвері (лише попередній перегляд).");
                return;
            }

            bool palmOk = _device.TrySetPalmConfig(requestedPalm, out var appliedPalm);
            if (palmOk)
            {
                LoadConfigIntoSliders(appliedPalm);
                DrawPreview();
            }

            bool pointerOk = _device.TrySetPointerConfig(requestedPointer, out var appliedPointer);
            if (pointerOk) LoadPointerConfigIntoControls(appliedPointer);

            bool scrollOk = _device.TrySetScrollConfig(requestedScroll, out var appliedScroll);
            if (scrollOk) LoadScrollConfigIntoControls(appliedScroll);

            if (palmOk && pointerOk && scrollOk)
                SetBottomStatus("Збережено в реєстрі драйвера (Palm + Scroll + Pointer).");
            else if (!palmOk && !pointerOk && !scrollOk)
                SetBottomStatus("Не вдалося зберегти налаштування (помилка DeviceIoControl).");
            else
                SetBottomStatus("Частину налаштувань не вдалося зберегти (помилка DeviceIoControl).");
        }

        private void ResetDefaults_Click(object sender, RoutedEventArgs e)
        {
            var appliedPalm = PalmConfig.Default;
            bool palmOk = _device.IsConnected && _device.TryResetPalmConfig(out appliedPalm);
            LoadConfigIntoSliders(palmOk ? appliedPalm : PalmConfig.Default);

            var appliedPointer = PointerConfig.Default;
            bool pointerOk = _device.IsConnected && _device.TryResetPointerConfig(out appliedPointer);
            LoadPointerConfigIntoControls(pointerOk ? appliedPointer : PointerConfig.Default);

            var appliedScroll = ScrollConfig.Default;
            bool scrollOk = _device.IsConnected && _device.TryResetScrollConfig(out appliedScroll);
            LoadScrollConfigIntoControls(scrollOk ? appliedScroll : ScrollConfig.Default);

            if (palmOk && pointerOk && scrollOk)
                SetBottomStatus("Скинуто до значень за замовчуванням (Palm + Scroll + Pointer).");
            else if (_device.IsConnected)
                SetBottomStatus("Не вдалося повністю скинути налаштування на драйвері — показано значення за замовчуванням.");
            else
                SetBottomStatus("Показано значення за замовчуванням (пристрій не підключено).");

            DrawPreview();
        }

        private void SaveProfile_Click(object sender, RoutedEventArgs e)
        {
            var dlg = new SaveFileDialog
            {
                Filter = "AmtPtp profile (*.json)|*.json",
                FileName = "AmtPtpProfile.json",
            };
            if (dlg.ShowDialog() != true) return;

            var profile = AmtPtpProfile.FromCurrent(
                ReadConfigFromSliders(),
                ReadPointerConfigFromControls(),
                ReadScrollConfigFromControls());

            var json = JsonSerializer.Serialize(profile, new JsonSerializerOptions { WriteIndented = true });
            File.WriteAllText(dlg.FileName, json);
            SetBottomStatus($"Профіль (Palm + Scroll + Pointer) збережено: {dlg.FileName}");
        }

        private void LoadProfile_Click(object sender, RoutedEventArgs e)
        {
            var dlg = new OpenFileDialog { Filter = "AmtPtp profile (*.json)|*.json" };
            if (dlg.ShowDialog() != true) return;

            try
            {
                var json = File.ReadAllText(dlg.FileName);

                // New-format files ({"Palm": {...}, "Pointer": {...}})
                // round-trip both configs. Files saved by an older GUI
                // build (before Pointer support existed) are just a bare
                // PalmConfig object with no wrapper - detect which shape
                // this file is and fall back so those old files still
                // load instead of throwing.
                bool isProfileFormat;
                using (var doc = JsonDocument.Parse(json))
                {
                    isProfileFormat = doc.RootElement.TryGetProperty("Palm", out _);
                }

                if (isProfileFormat)
                {
                    var profile = JsonSerializer.Deserialize<AmtPtpProfile>(json);
                    if (profile == null)
                        throw new InvalidDataException("Порожній або некоректний файл профілю.");

                    LoadConfigIntoSliders(profile.Palm.Clamped());
                    LoadPointerConfigIntoControls(profile.Pointer.Clamped());
                    LoadScrollConfigIntoControls(profile.Scroll == null ? ScrollConfig.Default : profile.Scroll.Clamped());
                    DrawPreview();
                    SetBottomStatus($"Профіль (Palm + Scroll + Pointer) завантажено: {dlg.FileName}. Натисніть «Зберегти», щоб записати на драйвер.");
                }
                else
                {
                    // Legacy flat-PalmConfig file - it never had Pointer
                    // data, so the Pointer tab's current values are left
                    // untouched rather than being reset to something the
                    // file never actually contained.
                    var cfg = JsonSerializer.Deserialize<PalmConfig>(json);
                    LoadConfigIntoSliders(cfg.Clamped());
                    DrawPreview();
                    SetBottomStatus($"Профіль (лише Palm, старий формат) завантажено: {dlg.FileName}. Натисніть «Зберегти», щоб записати на драйвер.");
                }
            }
            catch (Exception ex)
            {
                SetBottomStatus($"Помилка завантаження профілю: {ex.Message}");
            }
        }

        private void SetBottomStatus(string text) => BottomStatusText.Text = text;

        protected override void OnClosed(EventArgs e)
        {
            // Single teardown path (see the note in the constructor): tell
            // the driver to stop streaming live frames, stop the UI poll
            // timer, then release the device handle.
            if (_liveEnabled)
            {
                _device.SetLiveEnabled(false);
                _liveEnabled = false;
            }
            _liveTimer.Stop();
            _device.Dispose();
            base.OnClosed(e);
        }
    }
}