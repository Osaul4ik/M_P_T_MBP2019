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
    }


    internal sealed class WellspringBackup
    {
        public int Version { get; set; } = 1;
        public AppSettings AppSettings { get; set; } = new AppSettings();
        public List<GuiProfile> Profiles { get; set; } = new List<GuiProfile>();
        public int ActiveProfileIndex { get; set; }
    }

    internal static class AppSettingsStore
    {
        private static readonly string DirectoryPath = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "WellspringPTP");
        private static readonly string FilePath = Path.Combine(DirectoryPath, "settings.json");
        private static readonly JsonSerializerOptions JsonOptions = new JsonSerializerOptions
        {
            WriteIndented = true
        };

        public static AppSettings Load()
        {
            try
            {
                if (File.Exists(FilePath))
                {
                    var settings = JsonSerializer.Deserialize<AppSettings>(File.ReadAllText(FilePath), JsonOptions);
                    if (settings != null)
                        return settings;
                }
            }
            catch
            {
                // Fall back to defaults. A broken app-settings file must not
                // prevent the GUI from starting.
            }

            return new AppSettings();
        }

        public static void Save(AppSettings settings)
        {
            Directory.CreateDirectory(DirectoryPath);
            var json = JsonSerializer.Serialize(settings, JsonOptions);
            File.WriteAllText(FilePath, json);
        }
    }
}
