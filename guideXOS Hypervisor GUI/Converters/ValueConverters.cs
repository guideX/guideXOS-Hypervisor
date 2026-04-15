using System;
using System.Globalization;
using System.Windows;
using System.Windows.Data;

namespace guideXOS_Hypervisor_GUI.Converters
{
    /// <summary>
    /// Converts boolean to Visibility (inverse of standard BooleanToVisibilityConverter)
    /// True = Collapsed, False = Visible
    /// </summary>
    public class InverseBoolToVisibilityConverter : IValueConverter
    {
        public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
        {
            if (value is bool boolValue)
            {
                return boolValue ? Visibility.Collapsed : Visibility.Visible;
            }
            return Visibility.Visible;
        }

        public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
        {
            if (value is Visibility visibility)
            {
                return visibility != Visibility.Visible;
            }
            return false;
        }
    }

    /// <summary>
    /// Converts boolean ReadOnly property to Read/Write string
    /// True = "Read-Only", False = "Read/Write"
    /// </summary>
    public class BoolToReadWriteConverter : IValueConverter
    {
        public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
        {
            if (value is bool boolValue)
            {
                return boolValue ? "Read-Only" : "Read/Write";
            }
            return "Read/Write";
        }

        public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
        {
            throw new NotImplementedException();
        }
    }
    
    /// <summary>
    /// Converts percentage (0-100) to width for progress bars
    /// Parameter specifies the maximum width
    /// </summary>
    public class PercentageToWidthConverter : IValueConverter
    {
        public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
        {
            if (value is double percentage && parameter is string maxWidthStr)
            {
                if (double.TryParse(maxWidthStr, out double maxWidth))
                {
                    // Clamp percentage between 0 and 100
                    percentage = Math.Max(0, Math.Min(100, percentage));
                    return (percentage / 100.0) * maxWidth;
                }
            }
            return 0.0;
        }

        public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
        {
            throw new NotImplementedException();
        }
    }
}
