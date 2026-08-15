using System.Runtime.InteropServices;

namespace AmtPtpConfigGui.Native
{
    [StructLayout(LayoutKind.Sequential)]
    public struct PointerConfig
    {
        public uint StructVersion;
        public uint ForceTapThreshold;
        public uint ForceTapAction;
        public uint ForceTouchEnabled;
        public uint RequirePressureToActivate;
        public uint CursorSmoothingPercent;
        public uint CursorSpeedPercent;
        public uint CursorDeadzone;
        public uint CursorDeadzoneSlow;
        public uint CursorDeadzoneFast;
        public uint CursorSlowVelocity;
        public uint CursorFastVelocity;
        public uint SmoothingAlphaDen;
        public uint SmoothingAlphaNumSlow;
        public uint SmallContactRejectionEnabled;

        public const uint CurrentVersion = 5;
        public const uint ActionContextMenu = 0;
        public const uint ActionMiddleClick = 1;
        public const uint ActionDoubleClick = 2;
        public const uint ThresholdMin = 200;
        public const uint ThresholdMax = 400;
        public const uint ActionMax = 2;

        public static PointerConfig Default => new PointerConfig
        {
            StructVersion = CurrentVersion,
            ForceTapThreshold = 250,
            ForceTapAction = ActionContextMenu,
            ForceTouchEnabled = 1,
            RequirePressureToActivate = 1,
            CursorSmoothingPercent = 0,
            CursorSpeedPercent = 100,
            CursorDeadzone = 1,
            CursorDeadzoneSlow = 4,
            CursorDeadzoneFast = 0,
            CursorSlowVelocity = 110,
            CursorFastVelocity = 905,
            SmoothingAlphaDen = 8,
            SmoothingAlphaNumSlow = 3,
            SmallContactRejectionEnabled = 1,
        };

        public PointerConfig Clamped()
        {
            PointerConfig c = this;
            c.StructVersion = CurrentVersion;
            c.ForceTapThreshold = Clamp(c.ForceTapThreshold, ThresholdMin, ThresholdMax);
            if (c.ForceTapAction > ActionMax) c.ForceTapAction = ActionContextMenu;
            c.ForceTouchEnabled = c.ForceTouchEnabled != 0 ? 1u : 0u;
            c.RequirePressureToActivate = c.RequirePressureToActivate != 0 ? 1u : 0u;
            c.SmallContactRejectionEnabled = c.SmallContactRejectionEnabled != 0 ? 1u : 0u;
            c.CursorSmoothingPercent = Clamp(c.CursorSmoothingPercent, 0, 100);
            c.CursorSpeedPercent = Clamp(c.CursorSpeedPercent, 50, 200);
            c.CursorDeadzone = Clamp(c.CursorDeadzone, 0, 8);
            c.CursorDeadzoneSlow = Clamp(c.CursorDeadzoneSlow, 0, 8);
            c.CursorDeadzoneFast = Clamp(c.CursorDeadzoneFast, 0, 8);
            c.CursorSlowVelocity = Clamp(c.CursorSlowVelocity, 20, 300);
            c.CursorFastVelocity = Clamp(c.CursorFastVelocity, 200, 2000);
            if (c.CursorFastVelocity <= c.CursorSlowVelocity) c.CursorFastVelocity = c.CursorSlowVelocity + 1;
            c.SmoothingAlphaDen = Clamp(c.SmoothingAlphaDen, 1, 16);
            c.SmoothingAlphaNumSlow = Clamp(c.SmoothingAlphaNumSlow, 1, 16);
            if (c.SmoothingAlphaNumSlow > c.SmoothingAlphaDen) c.SmoothingAlphaNumSlow = c.SmoothingAlphaDen;
            return c;
        }

        private static uint Clamp(uint v, uint min, uint max) => v < min ? min : (v > max ? max : v);
    }
}
