using System.Runtime.InteropServices;

namespace AmtPtpConfigGui.Native
{
    /// <summary>
    /// Mirrors AMT_PALM_CONFIG from src/AmtPtpDeviceUsbKm/Public.h byte-for-byte.
    /// Every field is a plain uint (matches the kernel's ULONG), so there is
    /// no padding on either x86 or x64 - Marshal.SizeOf(typeof(PalmConfig))
    /// must equal sizeof(AMT_PALM_CONFIG) in the driver (56 bytes: 14 * 4).
    /// If you add a field on one side, add it on the other and keep them in
    /// the same order.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct PalmConfig
    {
        public uint StructVersion;

        public uint EdgePermilleTop;
        public uint EdgePermilleLeft;
        public uint EdgePermilleRight;
        public uint EdgePermilleBottom;

        public uint PalmLargeMajor;
        public uint PalmLargeRatio;
        public uint PalmScoreThresh;
        public uint PalmMinMajor;
        public uint PalmMinMinor;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 4)]
        public uint[] Reserved;

        public const uint CurrentVersion = 1;

        // Sane clamp bounds - must match AMT_PALM_*_MAX in Public.h.
        public const uint EdgePermilleMax = 400;
        public const uint MajorMax = 2000;
        public const uint RatioMax = 1000;
        public const uint ScoreMax = 200;

        /// <summary>
        /// Byte-for-byte the same values as AMT_PALM_CONFIG_DEFAULT_INIT in
        /// Public.h. Used when no device is connected (preview-only mode)
        /// and as the fallback if a GET IOCTL ever fails after connecting.
        /// </summary>
        public static PalmConfig Default => new PalmConfig
        {
            StructVersion = CurrentVersion,
            EdgePermilleTop = 36,
            EdgePermilleLeft = 143,
            EdgePermilleRight = 143,
            EdgePermilleBottom = 250,
            PalmLargeMajor = 380,
            PalmLargeRatio = 180,
            PalmScoreThresh = 55,
            PalmMinMajor = 80,
            PalmMinMinor = 40,
            Reserved = new uint[4],
        };

        public PalmConfig Clamped()
        {
            PalmConfig c = this;
            c.StructVersion = CurrentVersion;
            c.EdgePermilleTop = Clamp(c.EdgePermilleTop, 0, EdgePermilleMax);
            c.EdgePermilleLeft = Clamp(c.EdgePermilleLeft, 0, EdgePermilleMax);
            c.EdgePermilleRight = Clamp(c.EdgePermilleRight, 0, EdgePermilleMax);
            c.EdgePermilleBottom = Clamp(c.EdgePermilleBottom, 0, EdgePermilleMax);
            c.PalmLargeMajor = Clamp(c.PalmLargeMajor, 1, MajorMax);
            c.PalmLargeRatio = Clamp(c.PalmLargeRatio, 1, RatioMax);
            c.PalmScoreThresh = Clamp(c.PalmScoreThresh, 1, ScoreMax);
            c.PalmMinMajor = Clamp(c.PalmMinMajor, 0, MajorMax);
            c.PalmMinMinor = Clamp(c.PalmMinMinor, 0, MajorMax);
            c.Reserved ??= new uint[4];
            return c;
        }

        private static uint Clamp(uint v, uint min, uint max) => v < min ? min : (v > max ? max : v);
    }

    /// <summary>
    /// Mirrors AMT_PAD_GEOMETRY from Public.h. XMin/XMax/YMin/YMax are the
    /// raw sensor range reported by the currently-bound trackpad's
    /// BCM5974_CONFIG entry - the same units AMT_PALM_CONFIG's edge-permille
    /// fields are a percentage of.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct PadGeometry
    {
        public uint StructVersion;
        public int XMin;
        public int XMax;
        public int YMin;
        public int YMax;

        public const uint CurrentVersion = 1;

        public bool IsValid => XMax > XMin && YMax > YMin;

        /// <summary>Representative fallback geometry for preview-only mode
        /// (no device connected) - roughly a 2019 MacBook Pro 16" pad's raw
        /// sensor range. Only used to draw the preview canvas to a plausible
        /// aspect ratio; never sent to the driver.</summary>
        public static PadGeometry Fallback => new PadGeometry
        {
            StructVersion = CurrentVersion,
            XMin = -3678,
            XMax = 3934,
            YMin = -2478,
            YMax = 2587,
        };
    }
}
