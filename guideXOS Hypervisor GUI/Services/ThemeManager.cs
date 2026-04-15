using System;
using System.Windows;
using System.Windows.Media;

namespace guideXOS_Hypervisor_GUI.Services
{
    /// <summary>
    /// Manages application themes and provides dynamic theme switching
    /// </summary>
    public class ThemeManager
    {
        private static ThemeManager? _instance;
        private static readonly object _lock = new object();
        private ThemeType _currentTheme = ThemeType.Dark;
        private Color _accentColor = Color.FromRgb(0, 120, 215); // Default blue

        private ThemeManager()
        {
            // Private constructor for singleton
        }

        /// <summary>
        /// Singleton instance
        /// </summary>
        public static ThemeManager Instance
        {
            get
            {
                if (_instance == null)
                {
                    lock (_lock)
                    {
                        _instance ??= new ThemeManager();
                    }
                }
                return _instance;
            }
        }

        /// <summary>
        /// Current active theme
        /// </summary>
        public ThemeType CurrentTheme
        {
            get => _currentTheme;
            private set => _currentTheme = value;
        }

        /// <summary>
        /// Current accent color
        /// </summary>
        public Color AccentColor
        {
            get => _accentColor;
            private set => _accentColor = value;
        }

        /// <summary>
        /// Event raised when theme changes
        /// </summary>
        public event EventHandler<ThemeChangedEventArgs>? ThemeChanged;

        /// <summary>
        /// Apply dark theme to the application
        /// </summary>
        public void ApplyDarkTheme()
        {
            ApplyTheme(ThemeType.Dark);
        }

        /// <summary>
        /// Apply light theme to the application
        /// </summary>
        public void ApplyLightTheme()
        {
            ApplyTheme(ThemeType.Light);
        }

        /// <summary>
        /// Apply a specific theme
        /// </summary>
        /// <param name="theme">Theme to apply</param>
        public void ApplyTheme(ThemeType theme)
        {
            try
            {
                var oldTheme = CurrentTheme;
                CurrentTheme = theme;

                // Get the resource dictionary for the theme
                var themeUri = theme == ThemeType.Dark
                    ? new Uri("Themes/DarkTheme.xaml", UriKind.Relative)
                    : new Uri("Themes/LightTheme.xaml", UriKind.Relative);

                // Load the theme resource dictionary
                var themeDict = new ResourceDictionary { Source = themeUri };

                // Remove old theme dictionaries
                Application.Current.Resources.MergedDictionaries.Clear();

                // Add new theme dictionary
                Application.Current.Resources.MergedDictionaries.Add(themeDict);

                // Raise theme changed event
                ThemeChanged?.Invoke(this, new ThemeChangedEventArgs(oldTheme, theme));
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Failed to apply theme: {ex.Message}");
            }
        }

        /// <summary>
        /// Set a custom accent color for the theme
        /// </summary>
        /// <param name="color">The accent color</param>
        public void SetAccentColor(Color color)
        {
            try
            {
                AccentColor = color;

                // Update accent color resources
                var resources = Application.Current.Resources;

                // Update color resources
                if (resources.Contains("AccentBlue"))
                    resources["AccentBlue"] = color;

                if (resources.Contains("AccentBlueBrush"))
                    resources["AccentBlueBrush"] = new SolidColorBrush(color);

                // Calculate hover color (lighter)
                var hoverColor = Color.FromRgb(
                    (byte)Math.Min(255, color.R + 30),
                    (byte)Math.Min(255, color.G + 30),
                    (byte)Math.Min(255, color.B + 30));

                if (resources.Contains("AccentBlueHover"))
                    resources["AccentBlueHover"] = hoverColor;

                if (resources.Contains("AccentBlueHoverBrush"))
                    resources["AccentBlueHoverBrush"] = new SolidColorBrush(hoverColor);

                // Calculate pressed color (darker)
                var pressedColor = Color.FromRgb(
                    (byte)(color.R * 0.8),
                    (byte)(color.G * 0.8),
                    (byte)(color.B * 0.8));

                if (resources.Contains("AccentBluePressed"))
                    resources["AccentBluePressed"] = pressedColor;

                if (resources.Contains("AccentBluePressedBrush"))
                    resources["AccentBluePressedBrush"] = new SolidColorBrush(pressedColor);
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Failed to set accent color: {ex.Message}");
            }
        }

        /// <summary>
        /// Set accent color from hex string
        /// </summary>
        /// <param name="hexColor">Hex color string (e.g., "#007ACC")</param>
        public void SetAccentColor(string hexColor)
        {
            try
            {
                var color = (Color)ColorConverter.ConvertFromString(hexColor);
                SetAccentColor(color);
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Failed to parse color '{hexColor}': {ex.Message}");
            }
        }

        /// <summary>
        /// Toggle between dark and light themes
        /// </summary>
        public void ToggleTheme()
        {
            if (CurrentTheme == ThemeType.Dark)
                ApplyLightTheme();
            else
                ApplyDarkTheme();
        }

        /// <summary>
        /// Get a color resource from the current theme
        /// </summary>
        /// <param name="resourceKey">Resource key</param>
        /// <returns>Color or default if not found</returns>
        public Color GetColorResource(string resourceKey)
        {
            try
            {
                if (Application.Current.Resources.Contains(resourceKey))
                {
                    var resource = Application.Current.Resources[resourceKey];
                    if (resource is Color color)
                        return color;
                    if (resource is SolidColorBrush brush)
                        return brush.Color;
                }
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Failed to get color resource '{resourceKey}': {ex.Message}");
            }

            return Colors.Gray;
        }

        /// <summary>
        /// Get a brush resource from the current theme
        /// </summary>
        /// <param name="resourceKey">Resource key</param>
        /// <returns>Brush or default if not found</returns>
        public Brush GetBrushResource(string resourceKey)
        {
            try
            {
                if (Application.Current.Resources.Contains(resourceKey))
                {
                    var resource = Application.Current.Resources[resourceKey];
                    if (resource is Brush brush)
                        return brush;
                    if (resource is Color color)
                        return new SolidColorBrush(color);
                }
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Failed to get brush resource '{resourceKey}': {ex.Message}");
            }

            return Brushes.Gray;
        }

        /// <summary>
        /// Check if a resource exists in the current theme
        /// </summary>
        /// <param name="resourceKey">Resource key to check</param>
        /// <returns>True if resource exists</returns>
        public bool HasResource(string resourceKey)
        {
            return Application.Current.Resources.Contains(resourceKey);
        }
    }

    /// <summary>
    /// Theme types
    /// </summary>
    public enum ThemeType
    {
        Dark,
        Light
    }

    /// <summary>
    /// Event args for theme changed event
    /// </summary>
    public class ThemeChangedEventArgs : EventArgs
    {
        public ThemeType OldTheme { get; }
        public ThemeType NewTheme { get; }

        public ThemeChangedEventArgs(ThemeType oldTheme, ThemeType newTheme)
        {
            OldTheme = oldTheme;
            NewTheme = newTheme;
        }
    }
}
