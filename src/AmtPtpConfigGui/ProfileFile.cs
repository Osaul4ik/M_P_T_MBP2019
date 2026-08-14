namespace AmtPtpConfigGui.Native
{
    /// <summary>
    /// On-disk format for the GUI's "Зберегти/Завантажити профіль…" (.json)
    /// buttons. Bundles PalmConfig, ScrollConfig and PointerConfig into one file so
    /// a single Save/Load round-trips everything the global bottom-bar
    /// Save/Reset buttons touch - not just Palm.
    ///
    /// ProfileFormatVersion distinguishes this shape from the legacy
    /// (pre-Pointer-support) file format, which was just a bare PalmConfig
    /// JSON object with no wrapper - see MainWindow.LoadProfile_Click for
    /// the format-sniffing fallback that still loads those old files.
    /// </summary>
    public sealed class AmtPtpProfile
    {
        public const int CurrentFormatVersion = 3;

        public int ProfileFormatVersion { get; set; } = CurrentFormatVersion;
        public PalmConfig Palm { get; set; }
        public PointerConfig Pointer { get; set; }
        public ScrollConfig Scroll { get; set; }

        public static AmtPtpProfile FromCurrent(PalmConfig palm, PointerConfig pointer, ScrollConfig scroll) =>
            new AmtPtpProfile
            {
                ProfileFormatVersion = CurrentFormatVersion,
                Palm = palm,
                Pointer = pointer,
                Scroll = scroll,
            };
    }
}