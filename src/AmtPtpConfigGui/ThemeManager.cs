using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Media;

using WpfApplication = System.Windows.Application;
using WpfColor = System.Windows.Media.Color;
using WpfColorConverter = System.Windows.Media.ColorConverter;
using WpfPoint = System.Windows.Point;

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
            string ControlHover,
            string ControlPressed,
            string TintWarn,
            string TintError);

        private static readonly ThemeDefinition[] Definitions =
        {
            new("light", "Light",
                "#F5F7FA", "#171A1F",
                "#FFFFFF", "#EFF3F8", "#20242A",
                "#F7F9FC", "#3B82F6", "#2563EB",
                "#D8E6FB", "#3B82F6",
                "#EAF0F7", "#5C6673",
                "#F7D9D7", "#8A1C16",
                "#187A46",
                "#E9EEF5", "#C5CFDD", "#AEBAC9",
                "#E6ECF4", "#DCE5F0",
                "#12F0B06A", "#10E34B40"),

            new("dark", "Dark",
                "#111315", "#F5F7FA",
                "#1A1D21", "#24282E", "#E1E6ED",
                "#20252B", "#60A5FA", "#3B82F6",
                "#334155", "#AEB8C6",
                "#262B33", "#F3F6FA",
                "#452020", "#FFB4AE",
                "#4ADE80",
                "#20252B", "#4B5563", "#64748B",
                "#2A3038", "#333B46",
                "#14FFFFFF", "#16FF6B5A")
        };

        private const string DefaultTheme = "light";

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

            var resources = WpfApplication.Current.Resources;
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
            resources["AccentContentBrush"] = ContrastBrush(theme.Primary);
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
            resources["ControlHoverBrush"] = Brush(theme.ControlHover);
            resources["ControlPressedBrush"] = Brush(theme.ControlPressed);

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
                    "ScrollBarThumbHoverBrush", "ControlHoverBrush", "ControlPressedBrush"
                })
                {
                    localResources[key] = resources[key];
                }
            }
        }

        public static (System.Drawing.Color Back, System.Drawing.Color Fore, System.Drawing.Color Selected, System.Drawing.Color Border, System.Drawing.Color Accent) TrayColors(string? id)
        {
            var theme = Get(id);
            return (
                DrawingColor(theme.Surface),
                DrawingColor(theme.OnBg),
                DrawingColor(theme.SurfaceHover),
                DrawingColor(theme.Outline),
                DrawingColor(theme.Primary));
        }

        private static SolidColorBrush Brush(string hex) => Brush(hex, 0xFF);

        private static SolidColorBrush Brush(string hex, byte alpha)
        {
            var color = (WpfColor)WpfColorConverter.ConvertFromString(hex)!;
            color.A = alpha;
            var brush = new SolidColorBrush(color);
            brush.Freeze();
            return brush;
        }

        private static LinearGradientBrush Gradient(string startHex, string endHex)
        {
            var brush = new LinearGradientBrush
            {
                StartPoint = new WpfPoint(0, 0),
                EndPoint = new WpfPoint(1, 1)
            };
            brush.GradientStops.Add(new GradientStop((WpfColor)WpfColorConverter.ConvertFromString(startHex)!, 0));
            brush.GradientStops.Add(new GradientStop((WpfColor)WpfColorConverter.ConvertFromString(endHex)!, 1));
            brush.Freeze();
            return brush;
        }

        private static LinearGradientBrush GlassShine()
        {
            var brush = new LinearGradientBrush
            {
                StartPoint = new WpfPoint(0, 0),
                EndPoint = new WpfPoint(1, 1)
            };
            brush.GradientStops.Add(new GradientStop(WpfColor.FromArgb(0x40, 0xFF, 0xFF, 0xFF), 0));
            brush.GradientStops.Add(new GradientStop(WpfColor.FromArgb(0x00, 0xFF, 0xFF, 0xFF), 0.45));
            brush.Freeze();
            return brush;
        }

        private static SolidColorBrush ContrastBrush(string hex)
        {
            var c = (WpfColor)WpfColorConverter.ConvertFromString(hex)!;
            double luminance =
                0.2126 * (c.R / 255.0) +
                0.7152 * (c.G / 255.0) +
                0.0722 * (c.B / 255.0);

            var brush = new SolidColorBrush(
                luminance > 0.58
                    ? WpfColor.FromRgb(0x1A, 0x1C, 0x1E)
                    : WpfColor.FromRgb(0xFF, 0xFF, 0xFF));
            brush.Freeze();
            return brush;
        }

        private static string Darken(string hex)
        {
            var c = (WpfColor)WpfColorConverter.ConvertFromString(hex)!;
            return $"#{c.R:X2}{c.G:X2}{c.B:X2}";
        }

        private static System.Drawing.Color DrawingColor(string hex)
        {
            var c = (WpfColor)WpfColorConverter.ConvertFromString(hex)!;
            return System.Drawing.Color.FromArgb(c.A, c.R, c.G, c.B);
        }
    }
}