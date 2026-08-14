using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Media;

namespace AmtPtpConfigGui
{
    internal static class ThemeManager
    {
        internal sealed record ThemeDefinition(
            string Id,
            string Name,
            string Bg,
            string OnBg,
            string Surface,
            string SurfaceHover,
            string OnSurface,
            string SurfaceHigh,
            string Primary,
            string PrimaryHover,
            string Outline,
            string Shadow,
            string WarnBg,
            string WarnText,
            string ErrorBg,
            string ErrorText,
            string SuccessText,
            string ScrollTrack,
            string ScrollThumb,
            string ScrollThumbHover,
            string TintWarn,
            string TintError);

        private static readonly ThemeDefinition[] Definitions =
        {
            new("old", "Old", "#FFFBFF", "#1A1C1E", "#E1E6F0", "#D2D9E8", "#1A1C1E", "#F6F0D4", "#F6F0D4", "#F8EEC2", "#FCFAF1", "#33141E1E", "#F6F0D4", "#4A3200", "#F6F0D4", "#5C1408", "#1E7A34", "#E8E8E8", "#C7C7C7", "#B9B9B9", "#24FFBE5A", "#1FFF5A46"),
            new("cream", "Cream", "#FFF7E7", "#26190A", "#F2E2CC", "#E9D5B4", "#2B1C08", "#FFE7C4", "#FFE7C4", "#FCDBAA", "#FFF2DD", "#333C280A", "#FFE7C4", "#452C00", "#FFE7C4", "#5C1408", "#1E7A34", "#F5F3F0", "#D6C9B8", "#C1B09A", "#24FFB446", "#1FFF5A46"),
            new("paper", "Paper", "#FDF6E3", "#3A3226", "#F1EBD7", "#E9E1CA", "#3A3226", "#E3DBC7", "#E3DBC7", "#DED6BE", "#F7F1DE", "#32322D14", "#E3DBC7", "#3C2C00", "#E3DBC7", "#5C1408", "#1E7A34", "#EEE8D5", "#CCC5B9", "#BCB09F", "#22D2AA28", "#1FBE503C"),
            new("rose", "Rose", "#FFF0F6", "#3A0D21", "#F9D6E0", "#F0BFD4", "#3A0D21", "#F5B3CD", "#F5B3CD", "#EB8FBA", "#FDE9F3", "#33480A28", "#F5B3CD", "#442900", "#F5B3CD", "#5C1424", "#1E7A34", "#FFD6E8", "#F687B3", "#EC4899", "#FFFFF0F6", "#1FE63C64"),
            new("blue", "Blue", "#EEF4FF", "#0D1B2E", "#E3EAF5", "#D6E4FF", "#0D1B2E", "#C1D3F7", "#C1D3F7", "#A9C6F3", "#E6EDFB", "#330A143C", "#C1D3F7", "#402C00", "#C1D3F7", "#5C1408", "#1E7A34", "#E6EBF3", "#C2CCDA", "#AEB9C9", "#22F2F7FF", "#1FFF5A46"),
            new("monet", "Monet", "#FFF9F5", "#341A06", "#FFE0C2", "#FFCD9D", "#341A06", "#FFE0C0", "#FFE0C0", "#F7CDA2", "#FFF3E6", "#3346230A", "#FFE0C0", "#412A00", "#FFE0C0", "#5C1A08", "#1E7A34", "#F7F3EF", "#DEC8AA", "#D4B88E", "#1FFFF2EA", "#1FFF5A3C")
        };

        private const string DefaultTheme = "blue";

        public static string CurrentThemeId { get; private set; } = DefaultTheme;

        public static IReadOnlyList<ThemeDefinition> Themes => Definitions;

        public static ThemeDefinition Current => Get(CurrentThemeId);

        public static string NextThemeId(string current)
        {
            int index = Array.FindIndex(Definitions, t => string.Equals(t.Id, current, StringComparison.OrdinalIgnoreCase));
            if (index < 0) return Definitions[0].Id;
            return Definitions[(index + 1) % Definitions.Length].Id;
        }

        public static ThemeDefinition Get(string? id)
        {
            if (!string.IsNullOrWhiteSpace(id))
            {
                foreach (var theme in Definitions)
                {
                    if (string.Equals(theme.Id, id, StringComparison.OrdinalIgnoreCase))
                        return theme;
                }
            }

            return Definitions[0];
        }

        public static void Apply(string? id, ResourceDictionary? localResources = null)
        {
            var theme = Get(id);
            CurrentThemeId = theme.Id;

            var resources = Application.Current.Resources;
            resources["PageBrush"] = Brush(theme.Bg);
            resources["CardBrush"] = Brush(theme.Surface);
            resources["CardBorderBrush"] = Brush(theme.Outline);
            resources["TextPrimaryBrush"] = Brush(theme.OnBg);
            resources["TextSecondaryBrush"] = Brush(theme.OnSurface);
            resources["TextTertiaryBrush"] = Brush(theme.OnSurface, 0x99);
            resources["DividerBrush"] = Brush(theme.Outline);
            resources["ControlFillBrush"] = Brush(theme.SurfaceHigh);
            resources["ControlStrokeBrush"] = Brush(theme.Outline);
            resources["AccentBrush"] = Brush(theme.Primary);
            resources["AccentBrushHover"] = Brush(theme.PrimaryHover);
            resources["AccentBrushPressed"] = Brush(Darken(theme.PrimaryHover));
            resources["AccentSoftBrush"] = Brush(theme.SurfaceHigh);
            resources["DangerBrush"] = Brush(theme.ErrorText);
            resources["DangerSoftBrush"] = Brush(theme.ErrorBg);
            resources["SuccessBrush"] = Brush(theme.SuccessText);
            resources["SuccessSoftBrush"] = Brush(theme.SurfaceHigh);
            resources["BrandMarkBrush"] = Gradient(theme.PrimaryHover, theme.Primary);
            resources["BezelBrush"] = Gradient(theme.Surface, theme.SurfaceHigh);
            resources["GlassShineBrush"] = GlassShine();
            resources["WarnBrush"] = Brush(theme.WarnText);
            resources["WarnSoftBrush"] = Brush(theme.WarnBg);
            resources["ScrollBarTrackBrush"] = Brush(theme.ScrollTrack);
            resources["ScrollBarThumbBrush"] = Brush(theme.ScrollThumb);
            resources["ScrollBarThumbHoverBrush"] = Brush(theme.ScrollThumbHover);

            if (localResources != null && !ReferenceEquals(localResources, resources))
            {
                foreach (var key in new[]
                {
                    "PageBrush", "CardBrush", "CardBorderBrush", "TextPrimaryBrush",
                    "TextSecondaryBrush", "TextTertiaryBrush", "DividerBrush",
                    "ControlFillBrush", "ControlStrokeBrush", "AccentBrush",
                    "AccentBrushHover", "AccentBrushPressed", "AccentSoftBrush",
                    "DangerBrush", "DangerSoftBrush", "SuccessBrush", "SuccessSoftBrush",
                    "BrandMarkBrush", "BezelBrush", "GlassShineBrush", "WarnBrush",
                    "WarnSoftBrush", "ScrollBarTrackBrush", "ScrollBarThumbBrush",
                    "ScrollBarThumbHoverBrush"
                })
                {
                    localResources[key] = resources[key];
                }
            }
        }

        public static (System.Drawing.Color Back, System.Drawing.Color Fore, System.Drawing.Color Selected, System.Drawing.Color Border) TrayColors(string? id)
        {
            var theme = Get(id);
            return (
                DrawingColor(theme.Surface),
                DrawingColor(theme.OnBg),
                DrawingColor(theme.SurfaceHover),
                DrawingColor(theme.Outline));
        }

        private static SolidColorBrush Brush(string hex) => Brush(hex, 0xFF);

        private static SolidColorBrush Brush(string hex, byte alpha)
        {
            var color = (Color)ColorConverter.ConvertFromString(hex)!;
            color.A = alpha;
            var brush = new SolidColorBrush(color);
            brush.Freeze();
            return brush;
        }

        private static LinearGradientBrush Gradient(string startHex, string endHex)
        {
            var brush = new LinearGradientBrush
            {
                StartPoint = new Point(0, 0),
                EndPoint = new Point(1, 1)
            };
            brush.GradientStops.Add(new GradientStop((Color)ColorConverter.ConvertFromString(startHex)!, 0));
            brush.GradientStops.Add(new GradientStop((Color)ColorConverter.ConvertFromString(endHex)!, 1));
            brush.Freeze();
            return brush;
        }

        private static LinearGradientBrush GlassShine()
        {
            var brush = new LinearGradientBrush
            {
                StartPoint = new Point(0, 0),
                EndPoint = new Point(1, 1)
            };
            brush.GradientStops.Add(new GradientStop(Color.FromArgb(0x40, 0xFF, 0xFF, 0xFF), 0));
            brush.GradientStops.Add(new GradientStop(Color.FromArgb(0x00, 0xFF, 0xFF, 0xFF), 0.45));
            brush.Freeze();
            return brush;
        }

        private static string Darken(string hex)
        {
            var c = (Color)ColorConverter.ConvertFromString(hex)!;
            return $"#{c.R:X2}{c.G:X2}{c.B:X2}";
        }

        private static System.Drawing.Color DrawingColor(string hex)
        {
            var c = (Color)ColorConverter.ConvertFromString(hex)!;
            return System.Drawing.Color.FromArgb(c.A, c.R, c.G, c.B);
        }
    }
}
