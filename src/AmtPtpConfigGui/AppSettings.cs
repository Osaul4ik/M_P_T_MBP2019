using System;
using System.IO;
using System.Collections.Generic;
using System.Text.Json;

namespace AmtPtpConfigGui
{
    internal sealed class AppSettings
    {
        public bool CloseToTray { get; set; } = true;
        public bool StartWithWindows { get; set; } = false;
        public bool PalmEdgeRejectionEnabled { get; set; } = true;
        public string Theme { get; set; } = "light";
    }


    internal sealed class WellspringBackup
    {
        public int Version { get; set; } = 2;
        public AppSettings AppSettings { get; set; } = new AppSettings();
        public List<GuiProfile> Profiles { get; set; } = new List<GuiProfile>();
        public int ActiveProfileIndex { get; set; }
    }

    internal static class AppSettingsStore
    {
        private static readonly string DirectoryPath = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "WellspringPTP");
        private static readonly string FilePath = Path.Combine(DirectoryPath, "settings.json");
        private static readonly string TempFilePath = FilePath + ".tmp";
        private static readonly string BackupFilePath = FilePath + ".bak";
        private static readonly string CorruptBackupFilePath = FilePath + ".corrupt";
        private static readonly JsonSerializerOptions JsonOptions = new JsonSerializerOptions
        {
            WriteIndented = true
        };

        public static AppSettings Load()
        {
            foreach (string candidate in new[] { FilePath, BackupFilePath })
            {
                try
                {
                    if (!File.Exists(candidate))
                        continue;

                    var settings = JsonSerializer.Deserialize<AppSettings>(File.ReadAllText(candidate), JsonOptions);
                    if (settings != null)
                        return settings;
                }
                catch
                {
                    // Try the backup before falling back to defaults.
                }
            }

            try
            {
                if (File.Exists(FilePath))
                    File.Copy(FilePath, CorruptBackupFilePath, true);
            }
            catch
            {
            }

            return new AppSettings();
        }

        public static void Save(AppSettings settings)
        {
            Directory.CreateDirectory(DirectoryPath);
            var json = JsonSerializer.Serialize(settings, JsonOptions);

            try
            {
                using (var stream = new FileStream(
                    TempFilePath, FileMode.Create, FileAccess.Write, FileShare.None, 4096, FileOptions.WriteThrough))
                using (var writer = new StreamWriter(stream))
                {
                    writer.Write(json);
                    writer.Flush();
                    stream.Flush(true);
                }

                if (File.Exists(FilePath))
                    File.Replace(TempFilePath, FilePath, BackupFilePath, true);
                else
                    File.Move(TempFilePath, FilePath);
            }
            finally
            {
                try
                {
                    if (File.Exists(TempFilePath))
                        File.Delete(TempFilePath);
                }
                catch
                {
                }
            }
        }
    }
}