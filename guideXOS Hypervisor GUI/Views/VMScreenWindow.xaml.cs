using System;
using System.Windows;
using System.Windows.Input;
using guideXOS_Hypervisor_GUI.ViewModels;

namespace guideXOS_Hypervisor_GUI.Views
{
    /// <summary>
    /// Interaction logic for VMScreenWindow.xaml
    /// VM Screen Display Window
    /// </summary>
    public partial class VMScreenWindow : Window
    {
        private VMScreenViewModel? _viewModel;
        private bool _isFullscreen = false;
        private WindowState _previousWindowState;
        private WindowStyle _previousWindowStyle;
        private ResizeMode _previousResizeMode;

        public VMScreenWindow()
        {
            InitializeComponent();
        }

        public VMScreenWindow(string vmId, string vmName, bool isRunning = false) : this()
        {
            _viewModel = new VMScreenViewModel(vmId, vmName)
            {
                IsRunning = isRunning
            };
            
            DataContext = _viewModel;
            
            // Handle window close
            Closed += VMScreenWindow_Closed;
            
            // Handle keyboard shortcuts
            KeyDown += VMScreenWindow_KeyDown;
        }

        private void VMScreenWindow_Closed(object? sender, EventArgs e)
        {
            // Stop rendering when window closes
            _viewModel?.StopRendering();
        }

        private void VMScreenWindow_KeyDown(object sender, KeyEventArgs e)
        {
            // F11 or Alt+Enter to toggle fullscreen
            if (e.Key == Key.F11 || 
                (e.Key == Key.Enter && (Keyboard.Modifiers & ModifierKeys.Alt) == ModifierKeys.Alt))
            {
                ToggleFullscreen();
                e.Handled = true;
            }
            // Escape to exit fullscreen
            else if (e.Key == Key.Escape && _isFullscreen)
            {
                ToggleFullscreen();
                e.Handled = true;
            }
            // Ctrl+0 to reset scale
            else if (e.Key == Key.D0 && (Keyboard.Modifiers & ModifierKeys.Control) == ModifierKeys.Control)
            {
                _viewModel?.ResetScaleCommand.Execute(null);
                e.Handled = true;
            }
        }

        private void ToggleFullscreen()
        {
            if (!_isFullscreen)
            {
                // Enter fullscreen
                _previousWindowState = WindowState;
                _previousWindowStyle = WindowStyle;
                _previousResizeMode = ResizeMode;
                
                WindowStyle = WindowStyle.None;
                WindowState = WindowState.Maximized;
                ResizeMode = ResizeMode.NoResize;
                
                _isFullscreen = true;
            }
            else
            {
                // Exit fullscreen
                WindowStyle = _previousWindowStyle;
                WindowState = _previousWindowState;
                ResizeMode = _previousResizeMode;
                
                _isFullscreen = false;
            }
        }

        /// <summary>
        /// Update the VM running state
        /// </summary>
        public void UpdateRunningState(bool isRunning)
        {
            if (_viewModel != null)
            {
                _viewModel.IsRunning = isRunning;
            }
        }

        /// <summary>
        /// Get the ViewModel
        /// </summary>
        public VMScreenViewModel? ViewModel => _viewModel;
    }
}
