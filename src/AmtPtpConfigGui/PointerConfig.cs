using System.Runtime.InteropServices;

namespace AmtPtpConfigGui.Native
{
    [StructLayout(LayoutKind.Sequential)]
    public struct PointerConfig
    {
        public uint StructVersion;
        public uint ForceTapThreshold;
        public uint ForceTapAction;
        public uint CursorSmoothingPercent;
        public uint CursorSpeedPercent;
        public uint CursorDeadzone;
        public uint CursorSlowVelocity;
        public uint CursorFastVelocity;

        public const uint CurrentVersion = 2;
        public const uint ActionContextMenu = 0;
        public const uint ActionMiddleClick = 1;
        public const uint ActionDoubleClick = 2;
        public const uint ThresholdMin = 200;
        public const uint ThresholdMax = 400;
        public const uint ActionMax = 2;

        public static PointerConfig Default => new PointerConfig
        {
            StructVersion = CurrentVersion,
            ForceTapThreshold = 240,
            ForceTapAction = ActionContextMenu,
            CursorSmoothingPercent = 0,
            CursorSpeedPercent = 100,
            CursorDeadzone = 1,
            CursorSlowVelocity = 110,
            CursorFastVelocity = 700,
        };

        public PointerConfig Clamped()
        {
            PointerConfig c = this;
            c.StructVersion = CurrentVersion;
            c.ForceTapThreshold = Clamp(c.ForceTapThreshold, ThresholdMin, ThresholdMax);
            if (c.ForceTapAction > ActionMax) c.ForceTapAction = ActionContextMenu;
            c.CursorSmoothingPercent = Clamp(c.CursorSmoothingPercent, 0, 100);
            c.CursorSpeedPercent = Clamp(c.CursorSpeedPercent, 50, 200);
            c.CursorDeadzone = Clamp(c.CursorDeadzone, 0, 8);
            c.CursorSlowVelocity = Clamp(c.CursorSlowVelocity, 20, 300);
            c.CursorFastVelocity = Clamp(c.CursorFastVelocity, 200, 2000);
            if (c.CursorFastVelocity <= c.CursorSlowVelocity) c.CursorFastVelocity = c.CursorSlowVelocity + 1;
            return c;
        }

        private static uint Clamp(uint v, uint min, uint max) => v < min ? min : (v > max ? max : v);
    }
}
