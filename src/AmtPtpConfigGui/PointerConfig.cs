using System.Runtime.InteropServices;

namespace AmtPtpConfigGui.Native
{
    /// <summary>
    /// Mirrors AMT_POINTER_CONFIG from src/AmtPtpDeviceUsbKm/Public.h byte-for-byte.
    /// Every field is a plain uint (matches the kernel's ULONG), so there is
    /// no padding on either x86 or x64 - Marshal.SizeOf(typeof(PointerConfig))
    /// must equal sizeof(AMT_POINTER_CONFIG) in the driver (28 bytes: 7 * 4).
    /// If you add a field on one side, add it on the other and keep them in
    /// the same order.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct PointerConfig
    {
        public uint StructVersion;

        // Force Tap pressure threshold, raw ADC pressure units (~0-300).
        public uint ForceTapThreshold;

        // One of the Action* constants below.
        public uint ForceTapAction;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 5)]
        public uint[] Reserved;

        public const uint CurrentVersion = 1;

        // ForceTapAction values - must match AMT_POINTER_ACTION_* in Public.h.
        public const uint ActionContextMenu = 0; // синтетичний правий клік
        public const uint ActionMiddleClick = 1; // синтетична середня кнопка миші
        public const uint ActionDoubleClick = 2; // синтетичний подвійний клік (відкриття)

        // Sane clamp bounds - must match AMT_POINTER_THRESHOLD_*/ACTION_MAX in Public.h.
        public const uint ThresholdMin = 200;
        public const uint ThresholdMax = 400;
        public const uint ActionMax = 2;

        /// <summary>
        /// Byte-for-byte the same values as AMT_POINTER_CONFIG_DEFAULT_INIT in
        /// Public.h. Used when no device is connected (preview-only mode)
        /// and as the fallback if a GET IOCTL ever fails after connecting.
        /// </summary>
        public static PointerConfig Default => new PointerConfig
        {
            StructVersion = CurrentVersion,
            ForceTapThreshold = 240,
            ForceTapAction = ActionContextMenu,
            Reserved = new uint[5],
        };

        public PointerConfig Clamped()
        {
            PointerConfig c = this;
            c.StructVersion = CurrentVersion;
            c.ForceTapThreshold = Clamp(c.ForceTapThreshold, ThresholdMin, ThresholdMax);
            if (c.ForceTapAction > ActionMax)
                c.ForceTapAction = ActionContextMenu;
            c.Reserved ??= new uint[5];
            return c;
        }

        private static uint Clamp(uint v, uint min, uint max) => v < min ? min : (v > max ? max : v);
    }
}