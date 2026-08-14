using System;
using System.Globalization;
using System.Windows;
using System.Windows.Media;
using AmtPtpConfigGui.Native;

namespace AmtPtpConfigGui
{
    /// <summary>
    /// Single WPF visual for the live contact overlay.
    /// It avoids a per-contact visual tree and performs one render pass per UI frame.
    /// </summary>
    public sealed class LiveOverlayControl : FrameworkElement
    {
        private const int MaxContacts = 5;
        private const ushort PointOrientation = 16384;

        private readonly uint[] _smoothIds = new uint[MaxContacts];
        private readonly double[] _smoothMajor = new double[MaxContacts];
        private readonly double[] _smoothMinor = new double[MaxContacts];
        private readonly bool[] _seen = new bool[MaxContacts];

        private LiveFrame _frame;
        private PadGeometry _geometry;
        private double _geometrySmoothAlpha;
        private bool _hasFrame;

        private static readonly Typeface LabelTypeface =
            new Typeface(new FontFamily("Segoe UI"), FontStyles.Normal,
                         FontWeights.SemiBold, FontStretches.Normal);

        private static readonly Brush FingerFill = Frozen(70, 0x16, 0x9B, 0xFF);
        private static readonly Brush DownFill = Frozen(80, 0x1D, 0xF2, 0x75);
        private static readonly Brush UpFill = Frozen(70, 0xFF, 0xA0, 0x18);
        private static readonly Brush PalmFill = Frozen(120, 0xE8, 0x11, 0x23);
        private static readonly Brush FingerStroke = Frozen(0xFF, 0x00, 0xB7, 0xFF);
        private static readonly Brush PalmStroke = Frozen(0xFF, 0xFF, 0x3B, 0x45);
        private static readonly Brush TagBackground = Frozen(225, 0x14, 0x16, 0x1B);
        private static readonly Brush White = Brushes.White;

        private static Brush Frozen(byte a, byte r, byte g, byte b)
        {
            var brush = new SolidColorBrush(Color.FromArgb(a, r, g, b));
            brush.Freeze();
            return brush;
        }

        public void SetFrame(in LiveFrame frame, in PadGeometry geometry, double smoothingAlpha)
        {
            _frame = frame;
            _geometry = geometry;
            _geometrySmoothAlpha = smoothingAlpha;
            _hasFrame = true;
            InvalidateVisual();
        }

        public void Clear()
        {
            _hasFrame = false;
            for (int i = 0; i < MaxContacts; i++)
            {
                _smoothIds[i] = 0;
                _smoothMajor[i] = 0;
                _smoothMinor[i] = 0;
                _seen[i] = false;
            }
            InvalidateVisual();
        }

        protected override void OnRender(DrawingContext dc)
        {
            base.OnRender(dc);

            if (!_hasFrame || _frame.Contacts == null || _frame.ContactCount == 0)
                return;

            double w = ActualWidth > 0 ? ActualWidth : Width;
            double h = ActualHeight > 0 ? ActualHeight : Height;
            double xRange = _geometry.XMax - _geometry.XMin;
            double yRange = _geometry.YMax - _geometry.YMin;

            if (w <= 0 || h <= 0 || xRange <= 0 || yRange <= 0)
                return;

            Array.Clear(_seen, 0, _seen.Length);

            int count = Math.Min(_frame.ContactCount,
                Math.Min(_frame.Contacts.Length, MaxContacts));

            double sizeScale = ((w / xRange) + (h / yRange)) * 0.5;

            for (int i = 0; i < count; i++)
            {
                LiveContact c = _frame.Contacts[i];
                int smoothSlot = FindSmoothSlot(c.ContactID);
                _seen[smoothSlot] = true;

                double rawMajor = c.Major;
                double rawMinor = c.Minor;
                double major;
                double minor;

                if (c.Phase == 1 || _smoothIds[smoothSlot] != c.ContactID)
                {
                    major = rawMajor;
                    minor = rawMinor;
                }
                else
                {
                    major = _smoothMajor[smoothSlot] +
                            (rawMajor - _smoothMajor[smoothSlot]) * _geometrySmoothAlpha;
                    minor = _smoothMinor[smoothSlot] +
                            (rawMinor - _smoothMinor[smoothSlot]) * _geometrySmoothAlpha;
                }

                _smoothIds[smoothSlot] = c.ContactID;
                _smoothMajor[smoothSlot] = major;
                _smoothMinor[smoothSlot] = minor;

                double px = c.X / xRange * w;
                double py = c.Y / yRange * h;
                px = Math.Clamp(px, 0, w);
                py = Math.Clamp(py, 0, h);

                double majorPx = major > 0 ? Math.Max(6, major * sizeScale) : 12;
                double minorPx = minor > 0 ? Math.Max(6, minor * sizeScale) : 12;

                bool palm = c.PalmSuspect != 0;
                Brush fill = palm
                    ? PalmFill
                    : c.Phase == 1
                        ? DownFill
                        : c.Phase == 3
                            ? UpFill
                            : FingerFill;
                Brush stroke = palm ? PalmStroke : FingerStroke;

                double angle = 0;
                if (c.Orientation != PointOrientation)
                {
                    angle = -(c.Orientation * 90.0 / 16384.0);
                    while (angle <= -180) angle += 360;
                    while (angle > 180) angle -= 360;
                }

                dc.PushTransform(new RotateTransform(angle, px, py));
                dc.DrawEllipse(fill, new Pen(stroke, palm ? 2.5 : 2),
                    new Point(px, py), majorPx * 0.5, minorPx * 0.5);
                dc.Pop();

                dc.DrawEllipse(stroke, null, new Point(px, py), 3, 3);

                string tag = palm ? $"ID {c.ContactID} · PALM" : $"ID {c.ContactID}";
                string text = $"{tag}  P:{c.Pressure}  M:{c.Major}/{c.Minor}";

                double labelX = Math.Min(
                    Math.Max(4, px + Math.Max(18, majorPx * 0.5 + 4)),
                    Math.Max(4, w - 180));
                double labelY = Math.Clamp(py - 9, 2, Math.Max(2, h - 22));

                var formatted = new FormattedText(
                    text,
                    CultureInfo.InvariantCulture,
                    FlowDirection.LeftToRight,
                    LabelTypeface,
                    11,
                    palm ? White : stroke,
                    1.0);

                double padX = 5;
                double padY = 2;
                Rect bg = new Rect(
                    labelX - padX,
                    labelY - padY,
                    formatted.Width + padX * 2,
                    formatted.Height + padY * 2);

                dc.DrawRoundedRectangle(
                    TagBackground,
                    null,
                    bg,
                    4,
                    4);

                dc.DrawText(formatted, new Point(labelX, labelY));
            }

            for (int i = 0; i < MaxContacts; i++)
            {
                if (!_seen[i])
                {
                    _smoothIds[i] = 0;
                    _smoothMajor[i] = 0;
                    _smoothMinor[i] = 0;
                }
            }
        }

        private int FindSmoothSlot(uint id)
        {
            for (int i = 0; i < MaxContacts; i++)
            {
                if (_smoothIds[i] == id && id != 0)
                    return i;
            }

            for (int i = 0; i < MaxContacts; i++)
            {
                if (_smoothIds[i] == 0)
                    return i;
            }

            return 0;
        }
    }
}
