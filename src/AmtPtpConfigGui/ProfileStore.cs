using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;

namespace AmtPtpConfigGui
{
    internal sealed class GuiProfile
    {
        public string Name { get; set; } = "Profile 1";
        public Native.PalmConfig Palm { get; set; }
        public Native.PointerConfig Pointer { get; set; }
        public Native.ScrollConfig Scroll { get; set; }

        public override string ToString() => Name;
    }

    internal static class ProfileStore
    {
        private const int DefaultProfileCount = 3;
        private static readonly string DirectoryPath = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "WellspringPTP");
        private static readonly string FilePath = Path.Combine(DirectoryPath, "profiles.json");
        private static readonly JsonSerializerOptions JsonOptions = new JsonSerializerOptions
        {
            WriteIndented = true,
            IncludeFields = true
        };

        public static List<GuiProfile> Load()
        {
            try
            {
                if (File.Exists(FilePath))
                {
                    var json = File.ReadAllText(FilePath);
                    var profiles = JsonSerializer.Deserialize<List<GuiProfile>>(json, JsonOptions);
                    if (profiles != null && profiles.Count > 0)
                    {
                        Normalize(profiles);
                        return profiles;
                    }
                }
            }
            catch
            {
                // Corrupt/missing profile storage must not prevent the GUI from starting.
            }

            var defaults = new List<GuiProfile>();
            for (int i = 1; i <= DefaultProfileCount; i++)
            {
                defaults.Add(CreateDefault($"Profile {i}"));
            }
            Save(defaults);
            return defaults;
        }

        public static void Save(List<GuiProfile> profiles)
        {
            Directory.CreateDirectory(DirectoryPath);
            Normalize(profiles);
            var json = JsonSerializer.Serialize(profiles, JsonOptions);
            File.WriteAllText(FilePath, json);
        }

        public static GuiProfile CreateDefault(string name)
        {
            return new GuiProfile
            {
                Name = name,
                Palm = Native.PalmConfig.Default,
                Pointer = Native.PointerConfig.Default,
                Scroll = Native.ScrollConfig.Default,
            };
        }

        private static void Normalize(List<GuiProfile> profiles)
        {
            foreach (var p in profiles)
            {
                if (string.IsNullOrWhiteSpace(p.Name))
                    p.Name = "Profile";
                p.Palm = p.Palm.StructVersion == 0 ? Native.PalmConfig.Default : p.Palm.Clamped();
                p.Pointer = p.Pointer.StructVersion == 0 ? Native.PointerConfig.Default : p.Pointer.Clamped();
                p.Scroll = p.Scroll.StructVersion == 0 ? Native.ScrollConfig.Default : p.Scroll.Clamped();
            }
        }
    }
}
