using System;
using System.IO;

namespace AmtPtpConfigGui
{
    /// <summary>
    /// Shared location for settings and profiles for all users on the machine:
    /// %ProgramData%\WellspringPTP instead of the former per-user
    /// %APPDATA%\WellspringPTP. The installer grants the built-in Users group
    /// permission to modify this directory, so the non-elevated GUI can still
    /// read from and write to it.
    /// </summary>
    internal static class SettingsPaths
    {
        public static readonly string DirectoryPath = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "WellspringPTP");

        // Legacy per-user directory - used only as the source for a one-time
        // migration for users upgrading from an older GUI build.
        private static readonly string LegacyDirectoryPath = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "WellspringPTP");

        /// <summary>
        /// If <paramref name="fileName"/> does not yet exist in the new shared
        /// directory, but exists in the old per-user directory, copy it once.
        /// Best-effort: any error is ignored because a failed migration simply
        /// means starting with defaults, as with a clean installation.
        /// </summary>
        public static void MigrateLegacyFile(string fileName)
        {
            try
            {
                var newPath = Path.Combine(DirectoryPath, fileName);
                var legacyPath = Path.Combine(LegacyDirectoryPath, fileName);

                if (File.Exists(newPath) || !File.Exists(legacyPath))
                    return;

                Directory.CreateDirectory(DirectoryPath);
                File.Copy(legacyPath, newPath, overwrite: false);
            }
            catch
            {
                // Best effort - fall back to defaults/new file.
            }
        }
    }
}