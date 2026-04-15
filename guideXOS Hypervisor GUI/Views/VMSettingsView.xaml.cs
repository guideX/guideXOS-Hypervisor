using System.Windows;
using System.Windows.Controls;
using guideXOS_Hypervisor_GUI.Models;
using guideXOS_Hypervisor_GUI.ViewModels;

namespace guideXOS_Hypervisor_GUI.Views
{
    /// <summary>
    /// Interaction logic for VMSettingsView.xaml
    /// </summary>
    public partial class VMSettingsView : Window
    {
        private readonly VMSettingsViewModel _viewModel;

        public VMSettingsView(VMStateModel vmState)
        {
            InitializeComponent();

            _viewModel = new VMSettingsViewModel(vmState);
            DataContext = _viewModel;

            // Subscribe to events
            _viewModel.SaveRequested += ViewModel_SaveRequested;
            _viewModel.CancelRequested += ViewModel_CancelRequested;

            // Wire up category navigation
            SetupCategoryNavigation();
        }

        private void SetupCategoryNavigation()
        {
            // Find all category buttons and wire them up
            // Visual tree: Grid (Content) -> Grid (Row 0) -> Border (Column 0) -> StackPanel
            var mainContentGrid = (Grid)((Grid)Content).Children[0];
            var sidebarBorder = (Border)mainContentGrid.Children[0];
            var sidebar = (StackPanel)sidebarBorder.Child;
            
            foreach (var child in sidebar.Children)
            {
                if (child is Button button)
                {
                    button.Click += CategoryButton_Click;
                }
            }

            // Show General by default
            ShowCategory("General");
        }

        private void CategoryButton_Click(object sender, RoutedEventArgs e)
        {
            if (sender is Button button)
            {
                var category = button.Content.ToString();
                if (category != null)
                {
                    // Extract category name (remove emoji)
                    var parts = category.Split(' ');
                    if (parts.Length > 1)
                    {
                        category = parts[1];
                    }

                    ShowCategory(category);
                }
            }
        }

        private void ShowCategory(string category)
        {
            // Hide all category panels
            // Visual tree: Grid (Content) -> Grid (Row 0) -> ScrollViewer (Column 1) -> StackPanel
            var mainContentGrid = (Grid)((Grid)Content).Children[0];
            var scrollViewer = (ScrollViewer)mainContentGrid.Children[1];
            var mainStack = (StackPanel)scrollViewer.Content;

            GeneralSettings.Visibility = Visibility.Collapsed;
            SystemSettings.Visibility = Visibility.Collapsed;
            DisplaySettings.Visibility = Visibility.Collapsed;
            StorageSettings.Visibility = Visibility.Collapsed;
            NetworkSettings.Visibility = Visibility.Collapsed;
            PerformanceSettings.Visibility = Visibility.Collapsed;
            AdvancedSettings.Visibility = Visibility.Collapsed;

            // Show the selected category
            switch (category)
            {
                case "General":
                    GeneralSettings.Visibility = Visibility.Visible;
                    break;
                case "System":
                    SystemSettings.Visibility = Visibility.Visible;
                    break;
                case "Display":
                    DisplaySettings.Visibility = Visibility.Visible;
                    break;
                case "Storage":
                    StorageSettings.Visibility = Visibility.Visible;
                    break;
                case "Network":
                    NetworkSettings.Visibility = Visibility.Visible;
                    break;
                case "Performance":
                    PerformanceSettings.Visibility = Visibility.Visible;
                    break;
                case "Advanced":
                    AdvancedSettings.Visibility = Visibility.Visible;
                    break;
            }

            // Update selected category in view model
            _viewModel.SelectedCategory = category;

            // Scroll to top
            scrollViewer.ScrollToTop();
        }

        private void ViewModel_SaveRequested(object? sender, System.EventArgs e)
        {
            DialogResult = true;
            Close();
        }

        private void ViewModel_CancelRequested(object? sender, System.EventArgs e)
        {
            DialogResult = false;
            Close();
        }

        protected override void OnClosed(System.EventArgs e)
        {
            base.OnClosed(e);

            // Unsubscribe from events
            _viewModel.SaveRequested -= ViewModel_SaveRequested;
            _viewModel.CancelRequested -= ViewModel_CancelRequested;
        }

        /// <summary>
        /// Get the view model instance
        /// </summary>
        public VMSettingsViewModel ViewModel => _viewModel;
    }
}
