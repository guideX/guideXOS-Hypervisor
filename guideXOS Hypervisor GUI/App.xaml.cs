using System.Configuration;
using System.Data;
using System.Windows;
using guideXOS_Hypervisor_GUI.Services;

namespace guideXOS_Hypervisor_GUI
{
    /// <summary>
    /// Interaction logic for App.xaml
    /// </summary>
    public partial class App : Application
    {
        protected override void OnStartup(StartupEventArgs e)
        {
            base.OnStartup(e);

            // Initialize theme system
            InitializeTheme();
        }

        protected override void OnExit(ExitEventArgs e)
        {
            base.OnExit(e);
            
            // Ensure all VMs are saved when the application exits
            // This is a safety measure in addition to MainWindow's Closed event
            try
            {
                if (MainWindow?.DataContext is ViewModels.MainViewModel viewModel)
                {
                    viewModel.Cleanup();
                }
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Error saving VMs on exit: {ex.Message}");
            }
        }

        private void InitializeTheme()
        {
            // Apply dark theme by default (already loaded in App.xaml)
            ThemeManager.Instance.ApplyDarkTheme();

            // Optional: Set custom accent color
            // ThemeManager.Instance.SetAccentColor("#007ACC");
        }
    }

}
