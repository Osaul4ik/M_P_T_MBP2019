using System;
using System.Windows;
using System.Windows.Media;
using AmtPtpConfigGui.Native;

using MediaBrush = System.Windows.Media.Brush;
using MediaColor = System.Windows.Media.Color;
using MediaPen = System.Windows.Media.Pen;
using MediaPoint = System.Windows.Point;

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

        private static readonly MediaBrush FingerFill = Frozen(70, 0x16, 0x9B, 0xFF);
        private static readonly MediaBrush DownFill = Frozen(80, 0x1D, 0xF2, 0x75);
        private static readonly MediaBrush UpFill = Frozen(70, 0xFF, 0xA0, 0x18);
        private static readonly MediaBrush PalmFill = Frozen(120, 0xE8, 0x11, 0x23);
        private static readonly MediaBrush FingerStroke = Frozen(0xFF, 0x00, 0xB7, 0xFF);
        private static readonly MediaBrush PalmStroke = Frozen(0xFF, 0xFF, 0x3B, 0x45);

        private static MediaBrush Frozen(byte a, byte r, byte g, byte b)
        {
            var brush = new SolidColorBrush(MediaColor.FromArgb(a, r, g, b));
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
                MediaBrush fill = palm
                    ? PalmFill
                    : c.Phase == 1
                        ? DownFill
                        : c.Phase == 3
                            ? UpFill
                            : FingerFill;
                MediaBrush stroke = palm ? PalmStroke : FingerStroke;

                double angle = 0;
                if (c.Orientation != PointOrientation)
                {
                    angle = -(c.Orientation * 90.0 / 16384.0);
                    while (angle <= -180) angle += 360;
                    while (angle > 180) angle -= 360;
                }

                dc.PushTransform(new RotateTransform(angle, px, py));
                dc.DrawEllipse(fill, new MediaPen(stroke, palm ? 2.5 : 2),
                    new MediaPoint(px, py), majorPx * 0.5, minorPx * 0.5);
                dc.Pop();

                dc.DrawEllipse(stroke, null, new MediaPoint(px, py), 3, 3);

                // Contact details are rendered in the dedicated live text panel.
                // Keep the overlay visual-only to minimize WPF text/layout work.
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
