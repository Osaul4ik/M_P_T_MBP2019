using System.Runtime.InteropServices;

namespace AmtPtpConfigGui.Native
{
    [StructLayout(LayoutKind.Sequential)]
    public struct ScrollConfig
    {
        public uint StructVersion;
        public uint SpeedPercent;
        public uint FastSpeedPercent;
        public uint SmoothingPercent;
        public uint Deadzone;
        public uint FastVelocity;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 2)]
        public uint[] Reserved;

        public const uint CurrentVersion = 1;

        public static ScrollConfig Default => new ScrollConfig
        {
            StructVersion = CurrentVersion,
            SpeedPercent = 60,
            FastSpeedPercent = 100,
            SmoothingPercent = 0,
            Deadzone = 1,
            FastVelocity = 1600,
            Reserved = new uint[2],
        };

        public ScrollConfig Clamped()
        {
            ScrollConfig c = this;
            c.StructVersion = CurrentVersion;
            c.SpeedPercent = Clamp(c.SpeedPercent, 20, 200);
            c.FastSpeedPercent = Clamp(c.FastSpeedPercent, 20, 250);
            c.SmoothingPercent = Clamp(c.SmoothingPercent, 0, 100);
            c.Deadzone = Clamp(c.Deadzone, 0, 8);
            c.FastVelocity = Clamp(c.FastVelocity, 500, 4000);
            c.Reserved ??= new uint[2];
            return c;
        }

        private static uint Clamp(uint v, uint min, uint max) => v < min ? min : (v > max ? max : v);
    }
}
